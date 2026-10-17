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
#include "../pcsx2/USB/libretro-usb/desc.h"
#include "../pcsx2/USB/libretro-usb/usb-state.h"
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

/* The savestate block: the controller, a slot per port, then a footer of
 * size, version and magic. Fixed size, so retro_serialize_size does not
 * change with what is plugged in. */
#define USB_STATE_OHCI    10240
#define USB_STATE_SLOT    8192
#define USB_STATE_SIZE    (USB_STATE_OHCI + USB::NUM_PORTS * USB_STATE_SLOT + 16)
#define USB_STATE_VERSION 1
#define USB_STATE_MAGIC   "LRPS2USB"

enum UsbStateKind
{
	USB_KIND_NONE = 0,
	USB_KIND_KEYBOARD,
	USB_KIND_MOUSE,
	USB_KIND_EYETOY,
	USB_KIND_CAPTURE_EYE,
};

static u32 USBPortKind(u32 port)
{
	if (!s_usb_device[port])
		return USB_KIND_NONE;
	if ((int)port == s_camera_port)
		return s_camera == USB_CAMERA_EYETOY ? USB_KIND_EYETOY : USB_KIND_CAPTURE_EYE;
	return s_port_device[port] == USB_DEV_KEYBOARD ? USB_KIND_KEYBOARD : USB_KIND_MOUSE;
}

static void USBFreezeOHCI(UsbState& st)
{
	OHCIState* o = s_qemu_ohci;
	u32 fsmps = o->fsmps, fit = o->fit, fi = o->fi, frt = o->frt;
	u32 i;

	st.Do(&g_usb_last_cycle);
	st.Do(&s_usb_clocks);
	st.Do(&s_usb_remaining);
	st.Do(&o->eof_timer);
	st.Do(&o->sof_time);
	st.Do(&o->ctl);
	st.Do(&o->status);
	st.Do(&o->intr_status);
	st.Do(&o->intr);
	st.Do(&o->hcca);
	st.Do(&o->ctrl_head);
	st.Do(&o->ctrl_cur);
	st.Do(&o->bulk_head);
	st.Do(&o->bulk_cur);
	st.Do(&o->per_cur);
	st.Do(&o->done);
	st.Do(&o->done_count);
	st.Do(&fsmps);
	st.Do(&fit);
	st.Do(&fi);
	st.Do(&frt);
	st.Do(&o->frame_number);
	st.Do(&o->pstart);
	st.Do(&o->lst);
	st.Do(&o->rhdesc_a);
	st.Do(&o->rhdesc_b);
	for (i = 0; i < OHCI_MAX_PORTS; i++)
		st.Do(&o->rhport[i].ctrl);
	st.Do(&o->old_ctl);
	st.Bytes(o->usb_buf, sizeof(o->usb_buf));
	st.Do(&o->async_td);
	st.Do(&o->async_complete);
	if (st.reading)
	{
		o->fsmps = fsmps;
		o->fit   = fit;
		o->fi    = fi;
		o->frt   = frt;
	}
}

static void USBFreezeEndpoint(USBEndpoint* ep, UsbState& st)
{
	st.Do(&ep->pipeline);
	st.Do(&ep->halted);
	if (st.reading)
		while (!QTAILQ_EMPTY(&ep->queue))
			QTAILQ_REMOVE(&ep->queue, QTAILQ_FIRST(&ep->queue), queue);
}

/* What every USB device has: address, configuration, endpoints. */
static void USBFreezeDevice(USBDevice* dev, UsbState& st)
{
	int altsetting[USB_MAX_INTERFACES];
	u32 i;

	st.Do(&dev->speed);
	st.Do(&dev->addr);
	st.Do(&dev->state);
	st.Bytes(dev->setup_buf, sizeof(dev->setup_buf));
	st.Bytes(dev->data_buf, sizeof(dev->data_buf));
	st.Do(&dev->remote_wakeup);
	st.Do(&dev->setup_state);
	st.Do(&dev->setup_len);
	st.Do(&dev->setup_index);
	st.Do(&dev->configuration);
	if (st.reading && !st.error)
	{
		/* A configuration the descriptors do not have is a bad slot, not
		 * a device left on whatever it had before. */
		const int configuration = dev->configuration;
		usb_desc_set_config(dev, configuration);
		if (configuration != 0 && (!dev->config || dev->config->bConfigurationValue != configuration))
			st.error = true;
	}
	memcpy(altsetting, dev->altsetting, sizeof(altsetting));
	st.Bytes(altsetting, sizeof(altsetting));
	if (st.reading && !st.error)
		for (i = 0; i < (u32)dev->ninterfaces && i < USB_MAX_INTERFACES; i++)
		{
			if (usb_desc_set_interface(dev, i, altsetting[i]) < 0)
				st.error = true;
		}
	USBFreezeEndpoint(&dev->ep_ctl, st);
	for (i = 0; i < USB_MAX_ENDPOINTS; i++)
		USBFreezeEndpoint(&dev->ep_in[i], st);
	for (i = 0; i < USB_MAX_ENDPOINTS; i++)
		USBFreezeEndpoint(&dev->ep_out[i], st);
}

static void USBFreezePort(u32 port, u32 kind, UsbState& st)
{
	USBDevice* dev = s_usb_device[port];

	USBFreezeDevice(dev, st);
	if (kind == USB_KIND_EYETOY || kind == USB_KIND_CAPTURE_EYE)
		usb_eyetoy::eyetoy_freeze(dev, st);
	else
		usb_hid::usb_hid_freeze(dev, st);
}

static const u8* USBStateFind(const void* state, size_t size)
{
	const u8* end = (const u8*)state + size;
	u32 block_size, version;

	if (size < USB_STATE_SIZE)
		return nullptr;
	memcpy(&block_size, end - 16, 4);
	memcpy(&version, end - 12, 4);
	if (memcmp(end - 8, USB_STATE_MAGIC, 8) || block_size != USB_STATE_SIZE || version != USB_STATE_VERSION)
		return nullptr;
	return end - USB_STATE_SIZE;
}

size_t USBStateSize(void)
{
	return USB_STATE_SIZE;
}

void USBStateWrite(void* dst)
{
	u8* p = (u8*)dst;
	const u32 size = USB_STATE_SIZE, version = USB_STATE_VERSION;
	u32 port;

	memset(p, 0, USB_STATE_SIZE);
	if (!s_qemu_ohci)
		return;

	UsbState st = { p, USB_STATE_OHCI, 0, false, false };
	USBFreezeOHCI(st);
	for (port = 0; port < USB::NUM_PORTS && !st.error; port++)
	{
		UsbState slot = { p + USB_STATE_OHCI + port * USB_STATE_SLOT, USB_STATE_SLOT, 0, false, false };
		u32 kind = USBPortKind(port);
		slot.Do(&kind);
		if (kind != USB_KIND_NONE)
			USBFreezePort(port, kind, slot);
		st.error = slot.error;
	}
	if (st.error)
	{
		log_cb(RETRO_LOG_ERROR, "USB: the state does not fit its block\n");
		memset(p, 0, USB_STATE_SIZE);
		return;
	}
	memcpy(p + USB_STATE_SIZE - 16, &size, 4);
	memcpy(p + USB_STATE_SIZE - 12, &version, 4);
	memcpy(p + USB_STATE_SIZE - 8, USB_STATE_MAGIC, 8);
}

size_t USBStateStrip(const void* state, size_t size)
{
	return USBStateFind(state, size) ? size - USB_STATE_SIZE : size;
}

/* A state without the block, or one left mid-transfer, puts the devices
 * back as if plugged in afresh; a port whose device differs from the
 * state's is plugged in afresh too. */
void USBStateRead(const void* state, size_t size)
{
	const u8* p = USBStateFind(state, size);
	u32 port;

	if (!s_qemu_ohci)
		return;
	if (!p)
	{
		USBreset();
		return;
	}

	UsbState st = { const_cast<u8*>(p), USB_STATE_OHCI, 0, true, false };
	USBFreezeOHCI(st);
	if (st.error || s_qemu_ohci->async_td)
	{
		log_cb(RETRO_LOG_WARN, "USB: state not loaded; devices plugged in afresh\n");
		USBreset();
		return;
	}
	for (port = 0; port < USB::NUM_PORTS; port++)
	{
		UsbState slot = { const_cast<u8*>(p) + USB_STATE_OHCI + port * USB_STATE_SLOT, USB_STATE_SLOT, 0, true, false };
		u32 kind = USB_KIND_NONE;
		slot.Do(&kind);
		if (kind != USBPortKind(port))
		{
			/* Plugged in afresh: the port cycles and the device model goes
			 * back to its defaults, as a guest's port reset would do later. */
			if (s_usb_device[port])
				usb_port_reset(&GetOHCIPort(port).port);
			continue;
		}
		if (kind != USB_KIND_NONE)
		{
			USBFreezePort(port, kind, slot);
			/* A slot that ran short, or named a configuration or interface
			 * the device does not have, leaves the device half loaded:
			 * plugged in afresh instead. */
			if (slot.error)
			{
				log_cb(RETRO_LOG_WARN, "USB: port %u state not loaded; device plugged in afresh\n", port);
				usb_port_reset(&GetOHCIPort(port).port);
			}
		}
	}
}

USBDevice* USBGetPortDevice(unsigned port)
{
	return port < USB::NUM_PORTS ? s_usb_device[port] : nullptr;
}

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
