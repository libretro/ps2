/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#include <retro_atomic.h>
#include <chrono>
#include "common/Pcsx2Defs.h"

#if defined(__POSIX__)
#include <pthread.h>
#endif

#include <retro_timers.h>
#include "../../common/Threading.h"
#include "net.h"
#include "DEV9.h"
#ifdef _WIN32
#include "Win32/tap.h"
#endif
#ifdef HAVE_PCAP
#include "pcap_io.h"
#endif
#include "sockets.h"

#include "PacketReader/EthernetFrame.h"
#include "PacketReader/IP/IP_Packet.h"
#include "PacketReader/IP/UDP/UDP_Packet.h"

NetAdapter* nif;
Threading::Thread rx_thread;

Threading::Mutex rx_mutex;

/* RX pump thread loops on this; the control side flips it on
 * start/stop.  volatile gives neither atomicity nor ordering in
 * C++; use release/acquire atomics for the cross-thread handshake. */
static retro_atomic_int_t RxRunning;
//rx thread
void NetRxThread()
{
	NetPacket tmp;
	while (retro_atomic_load_acquire_int(&RxRunning))
	{
		while (rx_fifo_can_rx() && nif->recv(&tmp))
		{
			Threading::ScopedLock rx_lock(rx_mutex);
			//Check if we can still rx
			if (rx_fifo_can_rx())
				rx_process(&tmp);
			else
				Console.Error("DEV9: rx_fifo_can_rx() false after nif->recv(), dropping");
		}

		using namespace std::chrono_literals;
		retro_sleep(1);
	}
}

void tx_put(NetPacket* pkt)
{
	if (nif != nullptr)
		nif->send(pkt);
	//pkt must be copied if its not processed by here, since it can be allocated on the callers stack
}

void ad_reset()
{
	if (nif != nullptr)
		nif->reset();
}

NetAdapter* GetNetAdapter()
{
	NetAdapter* na = nullptr;

	switch (EmuConfig.DEV9.EthApi)
	{
#ifdef _WIN32
		case Pcsx2Config::DEV9Options::NetApi::TAP:
			na = static_cast<NetAdapter*>(new TAPAdapter());
			break;
#endif
		case Pcsx2Config::DEV9Options::NetApi::PCAP_Bridged:
		case Pcsx2Config::DEV9Options::NetApi::PCAP_Switched:
#ifdef HAVE_PCAP
			na = static_cast<NetAdapter*>(new PCAPAdapter());
			break;
#else
			Console.Error("DEV9: PCAP backend not built into this core, use the Sockets api");
			return 0;
#endif
		case Pcsx2Config::DEV9Options::NetApi::Sockets:
			na = static_cast<NetAdapter*>(new SocketAdapter());
			break;
		default:
			return 0;
	}

	if (!na->isInitialised())
	{
		delete na;
		return 0;
	}
	return na;
}

void InitNet()
{
	NetAdapter* na = GetNetAdapter();

	if (!na)
	{
		Console.Error("DEV9: Failed to GetNetAdapter()");
		EmuConfig.DEV9.EthEnable = false;
		return;
	}

	nif = na;
	retro_atomic_store_release_int(&RxRunning, 1);

	rx_thread.Start(NetRxThread);

	rx_thread.SetHighestPriority();
}

void ReconfigureLiveNet(const Pcsx2Config& old_config)
{
	//Eth
	if (EmuConfig.DEV9.EthEnable)
	{
		if (old_config.DEV9.EthEnable)
		{
			//Reload Net if adapter changed
			if (EmuConfig.DEV9.EthDevice != old_config.DEV9.EthDevice ||
				EmuConfig.DEV9.EthApi != old_config.DEV9.EthApi)
			{
				TermNet();
				InitNet();
				return;
			}
			else
				nif->reloadSettings();
		}
		else
			InitNet();
	}
	else if (old_config.DEV9.EthEnable)
		TermNet();
}

void TermNet()
{
	if (retro_atomic_load_acquire_int(&RxRunning))
	{
		retro_atomic_store_release_int(&RxRunning, 0);
		nif->close();
		Console.WriteLn("DEV9: Waiting for RX-net thread to terminate..");
		rx_thread.Join();
		Console.WriteLn("DEV9: Done");

		delete nif;
		nif = nullptr;
	}
}

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::UDP;

const IP_Address NetAdapter::internalIP{{{192, 0, 2, 1}}};
const MAC_Address NetAdapter::broadcastMAC{{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};
const MAC_Address NetAdapter::internalMAC{{{0x76, 0x6D, 0xF4, 0x63, 0x30, 0x31}}};

NetAdapter::NetAdapter()
{
	//Ensure eeprom matches our default
	SetMACAddress(nullptr);
}

bool NetAdapter::recv(NetPacket* pkt)
{
	if (!retro_atomic_load_acquire_int(&internalRxThreadRunning))
		return InternalServerRecv(pkt);
	return false;
}

bool NetAdapter::send(NetPacket* pkt)
{
	return InternalServerSend(pkt);
}

//RxRunning must be set false before this
NetAdapter::~NetAdapter()
{
	//unblock InternalServerRX thread
	if (retro_atomic_load_acquire_int(&internalRxThreadRunning))
	{
		retro_atomic_store_release_int(&internalRxThreadRunning, 0);

		{
			Threading::ScopedLock srvlock(internalRxMutex);
			internalRxHasData = true;
		}

		internalRxCV.Broadcast();
		internalRxThread.Join();
	}
}

void NetAdapter::InspectSend(NetPacket* pkt)
{
	if (EmuConfig.DEV9.EthLogDNS)
	{
		EthernetFrame frame(pkt);
		if (frame.protocol == (u16)EtherType::IPv4)
		{
			PayloadPtr* payload = static_cast<PayloadPtr*>(frame.GetPayload());
			IP_Packet ippkt(payload->data, payload->GetLength());

			if (ippkt.protocol == (u16)IP_Type::UDP)
			{
				IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
				UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

				if (udppkt.destinationPort == 53)
				{
					Console.WriteLn("DEV9: DNS: Packet Sent To %i.%i.%i.%i",
						ippkt.destinationIP.bytes[0], ippkt.destinationIP.bytes[1], ippkt.destinationIP.bytes[2], ippkt.destinationIP.bytes[3]);
					dnsLogger.InspectSend(&udppkt);
				}
			}
		}
	}
}
void NetAdapter::InspectRecv(NetPacket* pkt)
{
	if (EmuConfig.DEV9.EthLogDNS)
	{
		EthernetFrame frame(pkt);
		if (frame.protocol == (u16)EtherType::IPv4)
		{
			PayloadPtr* payload = static_cast<PayloadPtr*>(frame.GetPayload());
			IP_Packet ippkt(payload->data, payload->GetLength());

			if (ippkt.protocol == (u16)IP_Type::UDP)
			{
				IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
				UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

				if (udppkt.sourcePort == 53)
				{
					Console.WriteLn("DEV9: DNS: Packet Sent From %i.%i.%i.%i",
						ippkt.sourceIP.bytes[0], ippkt.sourceIP.bytes[1], ippkt.sourceIP.bytes[2], ippkt.sourceIP.bytes[3]);
					dnsLogger.InspectRecv(&udppkt);
				}
			}
		}
	}
}

void NetAdapter::SetMACAddress(MAC_Address* mac)
{
	if (mac == nullptr)
		ps2MAC = defaultMAC;
	else
		ps2MAC = *mac;

	*(MAC_Address*)&dev9.eeprom[0] = ps2MAC;

	//The checksum seems to be all the values of the mac added up in 16bit chunks
	dev9.eeprom[3] = (dev9.eeprom[0] + dev9.eeprom[1] + dev9.eeprom[2]) & 0xffff;
}

bool NetAdapter::VerifyPkt(NetPacket* pkt, int read_size)
{
	if ((*(MAC_Address*)&pkt->buffer[0] != ps2MAC) && (*(MAC_Address*)&pkt->buffer[0] != broadcastMAC))
	{
		//ignore strange packets
		return false;
	}

	if (*(MAC_Address*)&pkt->buffer[6] == ps2MAC)
	{
		//avoid pcap looping packets
		return false;
	}
	pkt->size = read_size;
	return true;
}

#ifdef _WIN32
void NetAdapter::InitInternalServer(PIP_ADAPTER_ADDRESSES adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOvveride)
#elif defined(__POSIX__)
void NetAdapter::InitInternalServer(ifaddrs* adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOvveride)
#endif
{
	if (adapter == nullptr)
		Console.Error("DEV9: InitInternalServer() got nullptr for adapter");

	dhcpOn = EmuConfig.DEV9.InterceptDHCP || dhcpForceEnable;
	if (dhcpOn)
		dhcpServer.Init(adapter, ipOverride, subnetOverride, gatewayOvveride);

	dnsServer.Init(adapter);

	if (blocks())
	{
		retro_atomic_store_release_int(&internalRxThreadRunning, 1);
		internalRxThread.Start([this]() { InternalServerThread(); });
	}
}

#ifdef _WIN32
void NetAdapter::ReloadInternalServer(PIP_ADAPTER_ADDRESSES adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOveride)
#elif defined(__POSIX__)
void NetAdapter::ReloadInternalServer(ifaddrs* adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOveride)
#endif
{
	if (adapter == nullptr)
		Console.Error("DEV9: ReloadInternalServer() got nullptr for adapter");

	dhcpOn = EmuConfig.DEV9.InterceptDHCP || dhcpForceEnable;
	if (dhcpOn)
		dhcpServer.Init(adapter, ipOverride, subnetOverride, gatewayOveride);

	dnsServer.Init(adapter);
}

bool NetAdapter::InternalServerRecv(NetPacket* pkt)
{
	IP_Payload* ippay;
	ippay = dhcpServer.Recv();
	if (ippay != nullptr)
	{
		IP_Packet* ippkt = new IP_Packet(ippay);
		ippkt->destinationIP = {{{255, 255, 255, 255}}};
		ippkt->sourceIP = internalIP;
		EthernetFrame frame(ippkt);
		frame.sourceMAC = internalMAC;
		frame.destinationMAC = ps2MAC;
		frame.protocol = (u16)EtherType::IPv4;
		frame.WritePacket(pkt);
		return true;
	}

	ippay = dnsServer.Recv();
	if (ippay != nullptr)
	{
		IP_Packet* ippkt = new IP_Packet(ippay);
		ippkt->destinationIP = ps2IP;
		ippkt->sourceIP = internalIP;
		EthernetFrame frame(ippkt);
		frame.sourceMAC = internalMAC;
		frame.destinationMAC = ps2MAC;
		frame.protocol = (u16)EtherType::IPv4;
		frame.WritePacket(pkt);
		InspectRecv(pkt);
		return true;
	}

	return false;
}

bool NetAdapter::InternalServerSend(NetPacket* pkt)
{
	EthernetFrame frame(pkt);
	if (frame.protocol == (u16)EtherType::IPv4)
	{
		PayloadPtr* payload = static_cast<PayloadPtr*>(frame.GetPayload());
		IP_Packet ippkt(payload->data, payload->GetLength());

		if (ippkt.protocol == (u16)IP_Type::UDP)
		{
			IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
			UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

			if (udppkt.destinationPort == 67)
			{
				//Send DHCP
				if (dhcpOn)
					return dhcpServer.Send(&udppkt);
			}
		}

		if (ippkt.destinationIP == internalIP)
		{
			if (ippkt.protocol == (u16)IP_Type::UDP)
			{
				ps2IP = ippkt.sourceIP;

				IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
				UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

				if (udppkt.destinationPort == 53)
				{
					//Send DNS
					return dnsServer.Send(&udppkt);
				}
			}
			return true;
		}
	}
	return false;
}

void NetAdapter::InternalSignalReceived()
{
	//Signal internal server thread to read
	if (retro_atomic_load_acquire_int(&internalRxThreadRunning))
	{
		{
			Threading::ScopedLock srvlock(internalRxMutex);
			internalRxHasData = true;
		}

		internalRxCV.Broadcast();
	}
}

void NetAdapter::InternalServerThread()
{
	NetPacket tmp;
	while (retro_atomic_load_acquire_int(&internalRxThreadRunning))
	{
		Threading::ScopedLock srvLock(internalRxMutex);
		while (!internalRxHasData)
			internalRxCV.Wait(internalRxMutex);

		{
			Threading::ScopedLock rx_lock(rx_mutex);
			while (rx_fifo_can_rx() && InternalServerRecv(&tmp))
				rx_process(&tmp);
		}

		internalRxHasData = false;
	}
}
