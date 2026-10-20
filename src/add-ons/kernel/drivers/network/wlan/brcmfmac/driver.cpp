/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 *
 * Device manager glue and network device interface of the brcmfmac driver.
 * The 802.11 configuration requests (SIOCG80211/SIOCS80211) are answered
 * directly, as the chip is a FullMAC device: scanning, joining and (where
 * the firmware supports it) the WPA handshake are done by its firmware.
 */


#include "brcmfmac.h"

#include <net/if.h>
#include <net/if_media.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sockio.h>

#include <ACPI.h>
#include "acpi.h"

#include <ether_driver.h>
#include <kernel.h>
#include <vm/vm.h>

extern "C" {
#	include <compat/sys/cdefs.h>
#	include <compat/sys/ioccom.h>
#	include <net80211/ieee80211_ioctl.h>
}


#define BRCMF_DRIVER_MODULE_NAME "drivers/network/brcmfmac/driver_v1"
#define BRCMF_DEVICE_MODULE_NAME "drivers/network/brcmfmac/device_v1"
#define BRCMF_DEVICE_ID_GENERATOR "brcmfmac/device_id"

// GPIO of the firmware controlled expander that drives WL_REG_ON
static const uint32 kWifiPowerGpio = 128 + 1;
static const uint32 kEmmcClockId = 1;
static const uint32 kFrameSize = 1514;

#define BRCMF_JOIN_KEY_PASSPHRASE	0x80000000


struct brcmf_info {
	device_node*	node;
	area_id			registerArea;
	uint32			irq;
	SdioHost*		host;
	Brcmf*			brcmf;
	mutex			startLock;
	int32			deviceId;
	bool			nonblocking;

	// state set by a host supplicant (wpa_supplicant's BSD driver)
	int32			roaming;
	int32			privacy;
	int32			wpa;
	int32			authMode;
	uint8			appIE[256];
	size_t			appIELength;
};


static device_manager_info* sDeviceManager;
net_buffer_module_info* gBufferModule;


// #pragma mark - 802.11 requests


static status_t
get_scan_results(brcmf_info* info, ieee80211req& request)
{
	size_t capacity = request.i_len;
	uint8* buffer = (uint8*)malloc(capacity > 0 ? capacity : 1);
	if (buffer == NULL)
		return B_NO_MEMORY;

	Brcmf* brcmf = info->brcmf;
	size_t used = 0;

	brcmf->LockState();
	for (int32 i = 0; i < brcmf->CountScanResults(); i++) {
		const brcmf_scan_entry* entry = brcmf->ScanResultAt(i);

		size_t length = sizeof(ieee80211req_scan_result) + entry->ssidLength
			+ entry->ieLength;
		length = (length + 3) & ~3;
		if (used + length > capacity)
			break;

		ieee80211req_scan_result* result
			= (ieee80211req_scan_result*)(buffer + used);
		memset(result, 0, length);
		result->isr_len = length;
		result->isr_ie_off = sizeof(ieee80211req_scan_result);
		result->isr_ie_len = entry->ieLength;
		uint16 channel = entry->channel;
		if (channel == 14)
			result->isr_freq = 2484;
		else if (channel < 14)
			result->isr_freq = 2407 + channel * 5;
		else
			result->isr_freq = 5000 + channel * 5;
		result->isr_noise = entry->noise;
		// Haiku interprets the RSSI as a signal quality percentage
		int32 quality = 2 * (entry->rssi + 100);
		result->isr_rssi = max_c(0, min_c(quality, 100));
		result->isr_intval = entry->beaconInterval;
		result->isr_capinfo = entry->capability;
		memcpy(result->isr_bssid, entry->bssid, 6);
		result->isr_nrates = min_c(entry->rateCount,
			(uint8)sizeof(result->isr_rates));
		memcpy(result->isr_rates, entry->rates, result->isr_nrates);
		result->isr_ssid_len = entry->ssidLength;
		result->isr_meshid_len = 0;
		uint8* data = (uint8*)(result + 1);
		memcpy(data, entry->ssid, entry->ssidLength);
		memcpy(data + entry->ssidLength, entry->ies, entry->ieLength);

		used += length;
	}
	brcmf->UnlockState();

	status_t status = B_OK;
	if (used > 0)
		status = user_memcpy(request.i_data, buffer, used);
	free(buffer);
	request.i_len = used;
	return status;
}


static status_t
handle_get_80211(brcmf_info* info, ieee80211req& request)
{
	Brcmf* brcmf = info->brcmf;

	switch (request.i_type) {
		case IEEE80211_IOC_SSID:
		{
			uint8 ssid[32];
			brcmf->LockState();
			size_t length = brcmf->IsLinkUp() ? brcmf->SsidLength() : 0;
			memcpy(ssid, brcmf->Ssid(), length);
			brcmf->UnlockState();
			if (length > request.i_len)
				length = request.i_len;
			request.i_len = length;
			if (length == 0)
				return B_OK;
			return user_memcpy(request.i_data, ssid, length);
		}

		case IEEE80211_IOC_BSSID:
		{
			uint8 bssid[6] = {};
			brcmf->LockState();
			if (brcmf->IsLinkUp())
				memcpy(bssid, brcmf->Bssid(), 6);
			brcmf->UnlockState();
			if (request.i_len < 6)
				return B_BAD_VALUE;
			request.i_len = 6;
			return user_memcpy(request.i_data, bssid, 6);
		}

		case IEEE80211_IOC_SCAN_RESULTS:
			return get_scan_results(info, request);

		case IEEE80211_IOC_ROAMING:
			// With an in-firmware supplicant, make wpa_supplicant fail to
			// attach to us, so that it uses IEEE80211_IOC_HAIKU_JOIN
			// instead, handing us just the PMK.
			if (brcmf->HasFirmwareSupplicant())
				return EOPNOTSUPP;
			request.i_val = info->roaming;
			return B_OK;
		case IEEE80211_IOC_PRIVACY:
			request.i_val = info->privacy;
			return B_OK;
		case IEEE80211_IOC_WPA:
			request.i_val = info->wpa;
			return B_OK;
		case IEEE80211_IOC_AUTHMODE:
			request.i_val = info->authMode;
			return B_OK;

		case IEEE80211_IOC_DEVCAPS:
		{
			uint32 caps[4];
			caps[0] = 0x00800000 | 0x01000000;	// IEEE80211_C_WPA1 | WPA2
			caps[1] = (1 << IEEE80211_CIPHER_WEP)
				| (1 << IEEE80211_CIPHER_TKIP)
				| (1 << IEEE80211_CIPHER_AES_CCM);
			caps[2] = 0;
			caps[3] = 0;
			size_t length = min_c((size_t)request.i_len, sizeof(caps));
			return user_memcpy(request.i_data, caps, length);
		}

		default:
			// This includes IEEE80211_IOC_ROAMING, which makes the
			// wpa_supplicant fall back to the IEEE80211_IOC_HAIKU_JOIN
			// request, leaving the handshake to the firmware.
			TRACE("unsupported 802.11 get request %u\n", request.i_type);
			return EOPNOTSUPP;
	}
}


static status_t
handle_set_80211(brcmf_info* info, ieee80211req& request)
{
	Brcmf* brcmf = info->brcmf;

	switch (request.i_type) {
		case IEEE80211_IOC_HAIKU_COMPAT_WLAN_UP:
		case IEEE80211_IOC_HAIKU_COMPAT_WLAN_DOWN:
			return B_OK;

		case IEEE80211_IOC_SCAN_REQ:
		{
			ieee80211_scan_req scan;
			memset(&scan, 0, sizeof(scan));
			if (request.i_data != NULL && request.i_len >= sizeof(scan)
				&& user_memcpy(&scan, request.i_data, sizeof(scan)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			const uint8* ssid = NULL;
			size_t ssidLength = 0;
			if (scan.sr_nssid > 0 && scan.sr_ssid[0].len > 0
				&& scan.sr_ssid[0].len <= IEEE80211_NWID_LEN) {
				ssid = scan.sr_ssid[0].ssid;
				ssidLength = scan.sr_ssid[0].len;
			}
			status_t status = brcmf->StartScan(ssid, ssidLength);
			if (status == B_BUSY)
				return EINPROGRESS;
			return status;
		}

		case IEEE80211_IOC_SSID:
		{
			uint8 ssid[IEEE80211_NWID_LEN];
			size_t length = min_c(request.i_len, sizeof(ssid));
			if (length == 0)
				return brcmf->Leave();
			if (user_memcpy(ssid, request.i_data, length) != B_OK)
				return B_BAD_ADDRESS;
			return brcmf->Join(ssid, length, 0, NULL, 0);
		}

		case IEEE80211_IOC_MLME:
		{
			ieee80211req_mlme mlme;
			if (request.i_len < sizeof(mlme)
				|| user_memcpy(&mlme, request.i_data, sizeof(mlme)) != B_OK) {
				return B_BAD_VALUE;
			}
			switch (mlme.im_op) {
				case IEEE80211_MLME_DEAUTH:
				case IEEE80211_MLME_DISASSOC:
					return brcmf->Leave();
				case IEEE80211_MLME_ASSOC:
				{
					size_t ssidLength = min_c(mlme.im_ssid_len,
						IEEE80211_NWID_LEN);
					if (info->wpa == 0 && info->appIELength == 0
						&& info->privacy == 0) {
						return brcmf->Join(mlme.im_ssid, ssidLength, 0, NULL,
							0);
					}
					return brcmf->JoinWithIE(mlme.im_ssid, ssidLength,
						mlme.im_macaddr, info->appIE, info->appIELength,
						info->privacy != 0);
				}
			}
			return EOPNOTSUPP;
		}

		case IEEE80211_IOC_ROAMING:
			info->roaming = request.i_val;
			return B_OK;
		case IEEE80211_IOC_PRIVACY:
			info->privacy = request.i_val;
			return B_OK;
		case IEEE80211_IOC_WPA:
			info->wpa = request.i_val;
			if (info->wpa == 0)
				info->appIELength = 0;
			return B_OK;
		case IEEE80211_IOC_AUTHMODE:
			info->authMode = request.i_val;
			return B_OK;
		case IEEE80211_IOC_DROPUNENCRYPTED:
		case IEEE80211_IOC_COUNTERMEASURES:
		case IEEE80211_IOC_DELKEY:
			return B_OK;

		case IEEE80211_IOC_APPIE:
		{
			if ((request.i_val & IEEE80211_APPIE_WPA) != IEEE80211_APPIE_WPA)
				return B_OK;
			size_t length = request.i_len;
			if (length > sizeof(info->appIE))
				return B_BAD_VALUE;
			if (length > 0
				&& user_memcpy(info->appIE, request.i_data, length) != B_OK) {
				return B_BAD_ADDRESS;
			}
			info->appIELength = length;
			return B_OK;
		}

		case IEEE80211_IOC_WPAKEY:
		{
			ieee80211req_key key;
			if (request.i_len < sizeof(key)
				|| user_memcpy(&key, request.i_data, sizeof(key)) != B_OK) {
				return B_BAD_VALUE;
			}
			if (key.ik_keylen > 32)
				return B_BAD_VALUE;
			static const uint8 kBroadcast[6]
				= { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
			bool group = (key.ik_flags & IEEE80211_KEY_GROUP) != 0
				|| memcmp(key.ik_macaddr, kBroadcast, 6) == 0;
			uint32 index = key.ik_keyix;
			if (index == (uint16)IEEE80211_KEYIX_NONE)
				index = 0;
			status_t status = brcmf->SetKey(key.ik_macaddr, group, index,
				key.ik_type, key.ik_keydata, key.ik_keylen, key.ik_keyrsc);
			memset(&key, 0, sizeof(key));
			return status;
		}

		case IEEE80211_IOC_HAIKU_JOIN:
		{
			if (request.i_len < sizeof(ieee80211_haiku_join_req)
				|| request.i_len > sizeof(ieee80211_haiku_join_req) + 128) {
				return B_BAD_VALUE;
			}
			uint8 buffer[sizeof(ieee80211_haiku_join_req) + 128];
			if (user_memcpy(buffer, request.i_data, request.i_len) != B_OK)
				return B_BAD_ADDRESS;
			ieee80211_haiku_join_req* join
				= (ieee80211_haiku_join_req*)buffer;
			size_t keyLength = join->i_key_len;
			if (sizeof(ieee80211_haiku_join_req) + keyLength > request.i_len)
				return B_BAD_VALUE;
			if (join->i_nwid_len == 0 || join->i_nwid_len > IEEE80211_NWID_LEN)
				return B_BAD_VALUE;

			// A binary PMK is always 32 bytes; a passphrase is flagged.
			if ((join->i_key_mode & BRCMF_JOIN_KEY_PASSPHRASE) == 0
				&& keyLength != 0 && keyLength != 32) {
				return B_BAD_VALUE;
			}
			status_t status = brcmf->Join(join->i_nwid, join->i_nwid_len,
				join->i_authentication_mode, join->i_key, keyLength);
			memset(buffer, 0, sizeof(buffer));
			return status;
		}

		default:
			TRACE("unsupported 802.11 set request %u\n", request.i_type);
			return EOPNOTSUPP;
	}
}


// #pragma mark - device hooks


static int32
brcmf_interrupt(void* data)
{
	brcmf_info* info = (brcmf_info*)data;
	if (!info->host->InterruptPending())
		return B_UNHANDLED_INTERRUPT;

	info->brcmf->InterruptReceived();
	return B_INVOKE_SCHEDULER;
}


static status_t
brcmf_init_device(void* _info, void** _cookie)
{
	*_cookie = _info;
	return B_OK;
}


static void
brcmf_uninit_device(void* _cookie)
{
}


static status_t
brcmf_open(void* _info, const char* path, int openMode, void** _cookie)
{
	brcmf_info* info = (brcmf_info*)_info;

	MutexLocker locker(info->startLock);
	if (!info->brcmf->IsRunning()) {
		status_t status = info->brcmf->Start();
		if (status != B_OK) {
			ERROR("cannot start the WLAN chip: %s\n", strerror(status));
			return status;
		}
	}

	info->nonblocking = (openMode & O_NONBLOCK) != 0;
	*_cookie = info;
	return B_OK;
}


static status_t
brcmf_close(void* cookie)
{
	return B_OK;
}


static status_t
brcmf_free(void* cookie)
{
	return B_OK;
}


static status_t
brcmf_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	brcmf_info* info = (brcmf_info*)cookie;
	Brcmf* brcmf = info->brcmf;

	switch (op) {
		case ETHER_INIT:
			return B_OK;

		case ETHER_GETADDR:
			return user_memcpy(buffer, brcmf->MacAddress(), 6);

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = kFrameSize;
			if (length != sizeof(frameSize))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (length != sizeof(value)
				|| user_memcpy(&value, buffer, sizeof(value)) != B_OK) {
				return B_BAD_VALUE;
			}
			info->nonblocking = value != 0;
			return B_OK;
		}

		case ETHER_SETPROMISC:
		{
			int32 value;
			if (length != sizeof(value)
				|| user_memcpy(&value, buffer, sizeof(value)) != B_OK) {
				return B_BAD_VALUE;
			}
			brcmf->SetPromiscuous(value != 0);
			return B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			// the firmware passes all multicast frames ("allmulti")
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
		{
			sem_id sem;
			if (user_memcpy(&sem, buffer, sizeof(sem_id)) != B_OK)
				return B_BAD_ADDRESS;
			brcmf->SetLinkStateSemaphore(sem);
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_IEEE80211;
			if (brcmf->IsLinkUp()) {
				state.media |= IFM_ACTIVE;
				state.quality = 1000;
				state.speed = 72000000;
			} else {
				state.quality = 0;
				state.speed = 0;
			}
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return brcmf->Send((net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return brcmf->Receive((net_buffer**)buffer, !info->nonblocking);

		case SIOCSIFMEDIA:
			// only station mode is supported
			return B_OK;

		case SIOCG80211:
		case SIOCS80211:
		{
			ieee80211req request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;

			status_t status;
			if (op == SIOCG80211)
				status = handle_get_80211(info, request);
			else
				status = handle_set_80211(info, request);
			if (status != B_OK)
				return status;

			if (op == SIOCG80211
				&& user_memcpy(buffer, &request, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


// #pragma mark - driver module


struct brcmf_crs {
	phys_addr_t	address;
	size_t		length;
	uint32		irq;
};


static acpi_status
brcmf_parse_crs(ACPI_RESOURCE* resource, void* context)
{
	brcmf_crs* crs = (brcmf_crs*)context;
	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			if (crs->address == 0) {
				crs->address = resource->Data.FixedMemory32.Address;
				crs->length = resource->Data.FixedMemory32.AddressLength;
			}
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			if (crs->address == 0) {
				crs->address = resource->Data.Memory32.Minimum;
				crs->length = resource->Data.Memory32.AddressLength;
			}
			break;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			if (crs->irq == 0 && resource->Data.ExtendedIrq.InterruptCount > 0)
				crs->irq = resource->Data.ExtendedIrq.Interrupts[0];
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			if (crs->irq == 0 && resource->Data.Irq.InterruptCount > 0)
				crs->irq = resource->Data.Irq.Interrupts[0];
			break;
	}
	return B_OK;
}


static float
brcmf_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "acpi") != 0) {
		return 0.0f;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, "BCM2847") != 0) {
		return 0.0f;
	}

	// The Arasan controller is only wired to the WLAN chip on the Raspberry
	// Pi 3 and 4 when the SD card uses the other controller.
	if (!is_raspberry_pi_firmware())
		return 0.0f;

	return 0.8f;
}


static status_t
brcmf_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Broadcom FullMAC SDIO WLAN" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, BRCMF_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static void
brcmf_free_info(brcmf_info* info)
{
	if (info->irq != 0 && info->host != NULL)
		remove_io_interrupt_handler(info->irq, brcmf_interrupt, info);
	delete info->brcmf;
	delete info->host;
	if (info->registerArea >= 0)
		delete_area(info->registerArea);
	mutex_destroy(&info->startLock);
	delete info;
}


static status_t
brcmf_init_driver(device_node* node, void** _cookie)
{
	brcmf_info* info = new(std::nothrow) brcmf_info;
	if (info == NULL)
		return B_NO_MEMORY;
	memset((void*)info, 0, sizeof(*info));
	info->node = node;
	info->registerArea = -1;
	info->deviceId = -1;
	mutex_init(&info->startLock, "brcmfmac start");

	device_node* acpiNode = sDeviceManager->get_parent_node(node);
	acpi_device_module_info* acpi;
	acpi_device device;
	sDeviceManager->get_driver(acpiNode, (driver_module_info**)&acpi,
		(void**)&device);
	sDeviceManager->put_node(acpiNode);

	brcmf_crs crs = {};
	status_t status = acpi->walk_resources(device, (ACPI_STRING)"_CRS",
		brcmf_parse_crs, &crs);
	if (status != B_OK || crs.address == 0) {
		ERROR("missing ACPI resources\n");
		brcmf_free_info(info);
		return B_DEVICE_NOT_FOUND;
	}
	if (crs.length < 0x100)
		crs.length = 0x100;

	void* registers;
	info->registerArea = map_physical_memory("brcmfmac sdio registers",
		crs.address, (crs.length + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1),
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&registers);
	if (info->registerArea < 0) {
		status = info->registerArea;
		brcmf_free_info(info);
		return status;
	}

	uint32 baseClock = vc_get_clock_rate(kEmmcClockId);
	uint32 caps = ((volatile uint32*)registers)[0x40 / 4];
	if (baseClock == 0)
		baseClock = ((caps >> 8) & 0xff) * 1000000;
	if (baseClock == 0)
		baseClock = 250000000;
	INFO("SDIO controller at %#" B_PRIxPHYSADDR ", irq %" B_PRIu32
		", version %#" B_PRIx32 ", base clock %" B_PRIu32 " MHz\n",
		crs.address, crs.irq,
		((volatile uint32*)registers)[0xfc / 4] >> 16, baseClock / 1000000);

	// Route the pins, and power cycle the chip via WL_REG_ON.
	bcm2711_wifi_pin_setup();
	vc_set_gpio(kWifiPowerGpio, 0);
	snooze(50000);
	status = vc_set_gpio(kWifiPowerGpio, 1);
	if (status != B_OK)
		ERROR("could not switch on WL_REG_ON: %s\n", strerror(status));
	snooze(150000);

	info->host = new(std::nothrow) SdioHost((addr_t)registers, baseClock);
	if (info->host == NULL) {
		brcmf_free_info(info);
		return B_NO_MEMORY;
	}
	info->brcmf = new(std::nothrow) Brcmf(info->host);
	if (info->brcmf == NULL) {
		brcmf_free_info(info);
		return B_NO_MEMORY;
	}

	status = info->brcmf->Probe();
	if (status != B_OK) {
		ERROR("WLAN chip probe failed: %s\n", strerror(status));
		info->irq = 0;
		brcmf_free_info(info);
		return B_DEVICE_NOT_FOUND;
	}

	if (crs.irq != 0) {
		info->irq = crs.irq;
		status = install_io_interrupt_handler(info->irq, brcmf_interrupt,
			info, 0);
		if (status != B_OK) {
			ERROR("cannot install interrupt handler, polling only\n");
			info->irq = 0;
		}
	}

	*_cookie = info;
	return B_OK;
}


static void
brcmf_uninit_driver(void* _cookie)
{
	brcmf_info* info = (brcmf_info*)_cookie;
	if (info->deviceId >= 0)
		sDeviceManager->free_id(BRCMF_DEVICE_ID_GENERATOR, info->deviceId);
	brcmf_free_info(info);
	vc_set_gpio(kWifiPowerGpio, 0);
}


static status_t
brcmf_register_child_devices(void* _cookie)
{
	brcmf_info* info = (brcmf_info*)_cookie;

	int32 id = sDeviceManager->create_id(BRCMF_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/brcmfmac/%" B_PRId32, id);
	info->brcmf->SetDeviceName(name);

	status_t status = sDeviceManager->publish_device(info->node, name,
		BRCMF_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(BRCMF_DEVICE_ID_GENERATOR, id);
	else
		info->deviceId = id;
	return status;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule },
	{}
};


static struct device_module_info sBrcmfDevice = {
	{
		BRCMF_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	brcmf_init_device,
	brcmf_uninit_device,
	NULL,	// remove

	brcmf_open,
	brcmf_close,
	brcmf_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	brcmf_ioctl,

	NULL,	// select
	NULL,	// deselect
};


static struct driver_module_info sBrcmfDriver = {
	{
		BRCMF_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	brcmf_supports_device,
	brcmf_register_device,
	brcmf_init_driver,
	brcmf_uninit_driver,
	brcmf_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sBrcmfDriver,
	(module_info*)&sBrcmfDevice,
	NULL
};
