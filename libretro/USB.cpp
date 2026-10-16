/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>

#include "../pcsx2/USB/USB.h"
#include "../pcsx2/USB/libretro-usb/USBinternal.h"
#include "../pcsx2/USB/libretro-usb/usb-hid.h"
#include "../pcsx2/USB/usb-eyetoy/usb-eyetoy-webcam.h"
#include "../pcsx2/SaveState.h"

#include "libretro.h"

/* IOP clock, 36.864 MHz. */
#define PSXCLK 36864000

/* OHCI host-controller clock-timing globals. Computed inside ohci_create()
 * from usb_get_ticks_per_second(); defined here so the controller core has
 * storage to link against. */
s64 g_usb_frame_time = 0;
s64 g_usb_bit_time   = 0;
s64 g_usb_last_cycle = 0;

static OHCIState* s_qemu_ohci   = nullptr;
static s64        s_usb_clocks  = 0;
static s64        s_usb_remaining = 0;

static int        s_port_device[USB::NUM_PORTS] = { USB_DEV_NONE, USB_DEV_NONE };
static unsigned   s_input_port[USB::NUM_PORTS]  = { 0, 1 };
static int        s_camera      = USB_CAMERA_NONE;
static int        s_camera_port = -1;
static USBDevice* s_usb_device[USB::NUM_PORTS]  = { nullptr, nullptr };

/* From PAD.cpp - the libretro input_state callback the frontend installed. */
extern retro_input_state_t PADGetInputStateCallback(void);

/* Set the device type for a USB port and which frontend controller port it
 * reads input from. Takes effect on the next USBopen()/USBreset(). For most
 * cases input_port == port; the combined keyboard+mouse selection routes both
 * USB ports to the same frontend port. */
void USBSetPortDevice(unsigned port, int device, unsigned input_port)
{
	if (port < USB::NUM_PORTS)
	{
		s_port_device[port] = device;
		s_input_port[port]  = input_port;
	}
}

static OHCIPort& GetOHCIPort(u32 port)
{
	const u32 rhport = (port < s_qemu_ohci->num_ports) ? port : 0;
	return s_qemu_ohci->rhport[rhport];
}

void USBSetCamera(int camera)
{
	s_camera = camera;
}

static void USBAttachDevice(u32 port, USBDevice* dev)
{
	GetOHCIPort(port).port.dev = dev;
	dev->attached = true;
	usb_attach(&GetOHCIPort(port).port);
	s_usb_device[port] = dev;
}

static void USBCreateDevice(u32 port)
{
	USBDevice* dev = nullptr;
	switch (s_port_device[port])
	{
		case USB_DEV_KEYBOARD:
			dev = usb_hid::usb_hid_create_kbd(port);
			break;
		case USB_DEV_MOUSE:
			dev = usb_hid::usb_hid_create_mouse(port);
			break;
		default:
			return;
	}
	if (dev)
		USBAttachDevice(port, dev);
}

static void USBCreateCamera(void)
{
	u32 port;
	USBDevice* dev;

	s_camera_port = -1;
	if (s_camera == USB_CAMERA_NONE)
		return;
	for (port = 0; port < USB::NUM_PORTS && s_usb_device[port]; port++)
		;
	if (port == USB::NUM_PORTS)
	{
		log_cb(RETRO_LOG_WARN, "USB: no free port for the camera\n");
		return;
	}
	dev = usb_eyetoy::eyetoy_create(
		s_camera == USB_CAMERA_EYETOY ? usb_eyetoy::TYPE_EYETOY : usb_eyetoy::TYPE_OV511P);
	if (!dev)
		return;
	USBAttachDevice(port, dev);
	s_camera_port = (int)port;
}

static void USBDestroyDevice(u32 port)
{
	USBDevice* dev = s_usb_device[port];
	if (!dev)
		return;
	if (dev->klass.unrealize)
		dev->klass.unrealize(dev);
	GetOHCIPort(port).port.dev = nullptr;
	s_usb_device[port] = nullptr;
}

int usb_get_ticks_per_second(void)
{
	return PSXCLK;
}

s64 usb_get_clock(void)
{
	return s_usb_clocks;
}

void USB::CheckForConfigChanges(const Pcsx2Config& old_config) { }

void USBconfigure(void) {}

void USBinit(void) {}

void USBshutdown(void) {}

bool USBopen(void)
{
	s_qemu_ohci = ohci_create(0x1f801600, 2);
	if (!s_qemu_ohci)
		return false;

	s_usb_clocks     = 0;
	s_usb_remaining  = 0;
	g_usb_last_cycle = 0;

	for (u32 port = 0; port < USB::NUM_PORTS; port++)
		USBCreateDevice(port);
	USBCreateCamera();
	return true;
}

void USBclose(void)
{
	for (u32 port = 0; port < USB::NUM_PORTS; port++)
		USBDestroyDevice(port);
	if (s_qemu_ohci)
	{
		free(s_qemu_ohci);
		s_qemu_ohci = nullptr;
	}
}

void USBreset(void)
{
	u32 port;

	s_usb_clocks     = 0;
	s_usb_remaining  = 0;
	g_usb_last_cycle = 0;
	if (!s_qemu_ohci)
		return;

	/* Reconcile attached devices with the current per-port selection, so a
	 * device-type change applied while running takes effect on reset. */
	for (port = 0; port < USB::NUM_PORTS; port++)
		USBDestroyDevice(port);

	ohci_hard_reset(s_qemu_ohci);

	for (port = 0; port < USB::NUM_PORTS; port++)
		USBCreateDevice(port);
	USBCreateCamera();
}

void USBasync(u32 cycles)
{
	if (!s_qemu_ohci)
		return;

	/* Pump one frame of frontend keyboard/mouse input into attached HID
	 * devices before advancing the controller. */
	{
		retro_input_state_t input_cb = PADGetInputStateCallback();
		u32 port;
		for (port = 0; port < USB::NUM_PORTS; port++)
		{
			if (s_usb_device[port] && (int)port != s_camera_port)
				usb_hid::usb_hid_update(s_usb_device[port], input_cb, s_input_port[port]);
		}
	}

	s_usb_remaining += cycles;
	s_usb_clocks    += s_usb_remaining;
	if (s_qemu_ohci->eof_timer > 0)
	{
		while ((u64)s_usb_remaining >= s_qemu_ohci->eof_timer)
		{
			s_usb_remaining -= s_qemu_ohci->eof_timer;
			s_qemu_ohci->eof_timer = 0;
			ohci_frame_boundary(s_qemu_ohci);

			/* Break out of the loop if the bus was stopped. */
			if (!s_qemu_ohci->eof_timer)
				break;
		}
		if ((s_usb_remaining > 0) && (s_qemu_ohci->eof_timer > 0))
		{
			s64 m = s_qemu_ohci->eof_timer;
			if (s_usb_remaining < m)
				m = s_usb_remaining;
			s_qemu_ohci->eof_timer -= m;
			s_usb_remaining        -= m;
		}
	}
}

s32 USBfreeze(FreezeAction mode, freezeData* data) { return 0; }

u8 USBread8(u32 addr) { return 0; }
u16 USBread16(u32 addr) { return 0; }

u32 USBread32(u32 addr)
{
	if (!s_qemu_ohci)
		return 0;
	return ohci_mem_read(s_qemu_ohci, addr);
}

void USBwrite8(u32 addr, u8 value) {}
void USBwrite16(u32 addr, u16 value) {}

void USBwrite32(u32 addr, u32 value)
{
	if (s_qemu_ohci)
		ohci_mem_write(s_qemu_ohci, addr, value);
}
