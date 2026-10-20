/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 *
 * Chip bring-up and firmware protocol for Broadcom/Cypress FullMAC SDIO WLAN
 * chips. Follows OpenBSD's bwfm(4) and Plan 9's ether4330 driver.
 */


#include "brcmfmac.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <NetworkNotifications.h>
#include <net_notifications.h>
#include <util/KMessage.h>


// #pragma mark - constants


// CCCR / FBR (function 0)
enum {
	CCCR_IOEN			= 0x02,
	CCCR_IORDY			= 0x03,
	CCCR_INTEN			= 0x04,
	CCCR_INTPEND		= 0x05,
	CCCR_IOABORT		= 0x06,
	CCCR_BUSIF			= 0x07,
	CCCR_CAPS			= 0x08,
	CCCR_SPEED			= 0x13,
	CCCR_BRCM_CARDCAP	= 0xf0,
	CCCR_BRCM_CARDCTRL	= 0xf1,
		CARDCTRL_WLANRESET	= 0x02,
	FBR_BLOCK_SIZE		= 0x10,		// + function * 0x100
};

// function 1 registers
enum {
	F1_WATERMARK		= 0x10008,
	F1_DEVICE_CTL		= 0x10009,
	F1_SBADDRLOW		= 0x1000a,
	F1_SBADDRMID		= 0x1000b,
	F1_SBADDRHIGH		= 0x1000c,
	F1_FRAMECTL			= 0x1000d,
		FRAMECTL_RF_TERM	= 0x01,
		FRAMECTL_WF_TERM	= 0x02,
	F1_CHIPCLKCSR		= 0x1000e,
		CLK_FORCE_ALP		= 0x01,
		CLK_FORCE_HT		= 0x02,
		CLK_FORCE_ILP		= 0x04,
		CLK_ALP_AVAIL_REQ	= 0x08,
		CLK_HT_AVAIL_REQ	= 0x10,
		CLK_FORCE_HW_CLKREQ_OFF = 0x20,
		CLK_ALP_AVAIL		= 0x40,
		CLK_HT_AVAIL		= 0x80,
	F1_SDIOPULLUP		= 0x1000f,
	F1_WFRMCNT			= 0x10019,
	F1_RFRMCNT			= 0x1001b,
	F1_SLEEPCSR			= 0x1001f,
		SLEEPCSR_KSO		= 0x01,
};

static const uint32 kSbWindowMask = 0x7fff;
static const uint32 kSbAccess32 = 0x8000;

// backplane
enum {
	CHIP_BASE			= 0x18000000,
	CHIP_REG_CHIPID		= 0x000,
	CHIP_REG_CAPS		= 0x004,
	CHIP_REG_CAPS_EXT	= 0x0ac,
	CHIP_REG_EROMPTR	= 0x0fc,
	CHIP_REG_PMUCONTROL	= 0x600,
	CHIP_REG_PMUCAPS	= 0x604,

	CAPS_PMU			= 0x10000000,
	CAPS_EXT_AOB		= 0x00000040,

	AGENT_IOCTL			= 0x408,
		IOCTL_CLK			= 0x0001,
		IOCTL_FGC			= 0x0002,
		IOCTL_CR4_CPUHALT	= 0x0020,
		IOCTL_D11_PHYCLOCKEN = 0x0004,
		IOCTL_D11_PHYRESET	= 0x0008,
	AGENT_RESET_CTL		= 0x800,
		RESET_CTL_RESET		= 0x0001,

	CORE_CHIPCOMMON		= 0x800,
	CORE_INTERNAL_MEM	= 0x80e,
	CORE_80211			= 0x812,
	CORE_PMU			= 0x827,
	CORE_SDIO_DEV		= 0x829,
	CORE_ARM_CM3		= 0x82a,
	CORE_ARM_CR4		= 0x83e,
	CORE_GCI			= 0x840,
	CORE_ARM_CA7		= 0x847,
	CORE_SYS_MEM		= 0x849,

	CR4_CAP				= 0x04,
	CR4_BANKIDX			= 0x40,
	CR4_BANKINFO		= 0x44,

	SOCRAM_COREINFO		= 0x00,
	SOCRAM_BANKIDX		= 0x10,
	SOCRAM_BANKINFO		= 0x40,
	SOCRAM_BANKPDA		= 0x44,
};

// EROM descriptors
enum {
	DMP_DESC_MASK			= 0x0000000f,
	DMP_DESC_COMPONENT		= 0x00000001,
	DMP_DESC_MASTER_PORT	= 0x00000003,
	DMP_DESC_ADDRESS		= 0x00000005,
	DMP_DESC_ADDRSIZE_GT32	= 0x00000008,
	DMP_DESC_EOT			= 0x0000000f,
	DMP_COMP_PARTNUM		= 0x000fff00,
	DMP_COMP_PARTNUM_S		= 8,
	DMP_COMP_REVISION		= 0xff000000,
	DMP_COMP_REVISION_S		= 24,
	DMP_COMP_NUM_SWRAP		= 0x00f80000,
	DMP_COMP_NUM_SWRAP_S	= 19,
	DMP_COMP_NUM_MWRAP		= 0x0007c000,
	DMP_COMP_NUM_MWRAP_S	= 14,
	DMP_SLAVE_ADDR_BASE		= 0xfffff000,
	DMP_SLAVE_TYPE			= 0x000000c0,
	DMP_SLAVE_TYPE_S		= 6,
	DMP_SLAVE_TYPE_SLAVE	= 0,
	DMP_SLAVE_TYPE_SWRAP	= 2,
	DMP_SLAVE_TYPE_MWRAP	= 3,
	DMP_SLAVE_SIZE_TYPE		= 0x00000030,
	DMP_SLAVE_SIZE_TYPE_S	= 4,
	DMP_SLAVE_SIZE_4K		= 0,
	DMP_SLAVE_SIZE_8K		= 1,
	DMP_SLAVE_SIZE_DESC		= 3,
};

// SDIO device core registers
enum {
	SD_INTSTATUS			= 0x020,
		INTSTATUS_HMB_FC_STATE	= 1 << 4,
		INTSTATUS_HMB_FC_CHANGE	= 1 << 5,
		INTSTATUS_HMB_FRAME_IND	= 1 << 6,
		INTSTATUS_HMB_HOST_INT	= 1 << 7,
		INTSTATUS_HMB_SW_MASK	= 0x000000f0,
		INTSTATUS_CHIPACTIVE	= 1 << 29,
	SD_HOSTINTMASK			= 0x024,
	SD_TOSBMAILBOX			= 0x040,
		TOSBMAILBOX_INT_ACK		= 1 << 1,
	SD_TOSBMAILBOXDATA		= 0x048,
	SD_TOHOSTMAILBOXDATA	= 0x04c,
		HOSTMB_NAKHANDLED		= 1 << 0,
		HOSTMB_DEVREADY			= 1 << 1,
		HOSTMB_FC				= 1 << 2,
		HOSTMB_FWREADY			= 1 << 3,
		HOSTMB_FWHALT			= 1 << 4,
};

static const uint32 kSdpcmProtocolVersion = 4;

// SDPCM channels
enum {
	CHANNEL_CONTROL		= 0,
	CHANNEL_EVENT		= 1,
	CHANNEL_DATA		= 2,
	CHANNEL_GLOM		= 3,
};

static const size_t kSdpcmHeaderSize = 12;
static const size_t kBcdcHeaderSize = 4;
static const size_t kDcmdHeaderSize = 16;
static const size_t kMaxDcmdPayload = 8192;

// dcmd commands
enum {
	C_UP					= 2,
	C_SET_PROMISC			= 10,
	C_SET_INFRA				= 20,
	C_GET_BSSID				= 23,
	C_SET_SSID				= 26,
	C_SET_PASSIVE_SCAN		= 49,
	C_DISASSOC				= 52,
	C_SET_PM				= 86,
	C_SET_SCAN_CHANNEL_TIME	= 185,
	C_SET_SCAN_UNASSOC_TIME	= 187,
	C_GET_VAR				= 262,
	C_SET_VAR				= 263,
	C_SET_SCAN_PASSIVE_TIME	= 258,
	C_SET_WSEC_PMK			= 268,
};

enum {
	DCMD_ERROR			= 1 << 0,
	DCMD_SET			= 1 << 1,
};

// events
enum {
	E_SET_SSID			= 0,
	E_JOIN				= 1,
	E_AUTH				= 3,
	E_DEAUTH			= 5,
	E_DEAUTH_IND		= 6,
	E_ASSOC				= 7,
	E_REASSOC			= 9,
	E_DISASSOC			= 11,
	E_DISASSOC_IND		= 12,
	E_LINK				= 16,
	E_ROAM				= 19,
	E_PSK_SUP			= 46,
	E_ESCAN_RESULT		= 69,
};

enum {
	E_STATUS_SUCCESS		= 0,
	E_STATUS_FAIL			= 1,
	E_STATUS_TIMEOUT		= 2,
	E_STATUS_NO_NETWORKS	= 3,
	E_STATUS_ABORT			= 4,
	E_STATUS_FWSUP_COMPLETED = 6,
	E_STATUS_PARTIAL		= 8,
};

// security
enum {
	WPA_AUTH_DISABLED		= 0,
	WPA_AUTH_WPA_UNSPECIFIED	= 1 << 1,
	WPA_AUTH_WPA_PSK		= 1 << 2,
	WPA_AUTH_WPA2_UNSPECIFIED	= 1 << 6,
	WPA_AUTH_WPA2_PSK		= 1 << 7,
	WSEC_NONE				= 0,
	WSEC_WEP				= 1 << 0,
	WSEC_TKIP				= 1 << 1,
	WSEC_AES				= 1 << 2,
	WSEC_PASSPHRASE			= 1 << 0,
};

// Haiku-private flag in ieee80211_haiku_join_req::i_key_mode: the key is a
// passphrase rather than a PMK.
#define BRCMF_JOIN_KEY_PASSPHRASE	0x80000000


static inline uint16
get16(const uint8* p)
{
	return p[0] | (p[1] << 8);
}


static inline uint32
get32(const uint8* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24);
}


static inline uint32
get32be(const uint8* p)
{
	return ((uint32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}


static inline uint16
get16be(const uint8* p)
{
	return (p[0] << 8) | p[1];
}


static inline void
put16(uint8* p, uint16 v)
{
	p[0] = v;
	p[1] = v >> 8;
}


static inline void
put32(uint8* p, uint32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}


static inline size_t
round_up(size_t value, size_t to)
{
	return (value + to - 1) / to * to;
}


static net_notifications_module_info* sNotificationModule;


// #pragma mark - Brcmf


Brcmf::Brcmf(SdioHost* host)
	:
	fHost(host),
	fWindow(0xffffffff),
	fChip(0),
	fChipRev(0),
	fCoreCount(0),
	fRamBase(0),
	fRamSize(0),
	fCcCaps(0),
	fCcCapsExt(0),
	fSdioCoreBase(0),
	fBounce(NULL),
	fTxBuffer(NULL),
	fRunning(false),
	fProbed(false),
	fReaderThread(-1),
	fStopReader(false),
	fInterruptSem(-1),
	fTxSeq(0),
	fTxMax(0),
	fFlowControl(0),
	fRequestId(0),
	fPendingId(0),
	fReply(NULL),
	fReplyLength(0),
	fReplyDone(false),
	fRxHead(0),
	fRxCount(0),
	fLinkUp(false),
	fAssociated(false),
	fUsingSupplicant(false),
	fKeyed(false),
	fSsidLength(0),
	fRssi(0),
	fScanVersion(0),
	fHasFirmwareSupplicant(false),
	fScanning(false),
	fScanCount(0),
	fLinkStateSem(-1)
{
	mutex_init(&fBusLock, "brcmfmac bus");
	mutex_init(&fStateLock, "brcmfmac state");
	mutex_init(&fCommandLock, "brcmfmac command");
	fTxCondition.Init(this, "brcmfmac tx");
	fReplyCondition.Init(this, "brcmfmac reply");
	fRxCondition.Init(this, "brcmfmac rx");
	B_INITIALIZE_SPINLOCK(&fRxLock);
	memset(fMacAddress, 0, sizeof(fMacAddress));
	memset(fBssid, 0, sizeof(fBssid));
	memset(fSsid, 0, sizeof(fSsid));
	memset(fScanResults, 0, sizeof(fScanResults));
	strlcpy(fDeviceName, "net/brcmfmac/0", sizeof(fDeviceName));
	fFirmwareBase[0] = '\0';

	fBounce = (uint8*)malloc(BRCMF_BUFFER_SIZE);
	fTxBuffer = (uint8*)malloc(BRCMF_BUFFER_SIZE);
	fReply = (uint8*)malloc(kDcmdHeaderSize + kMaxDcmdPayload);
	fInterruptSem = create_sem(0, "brcmfmac interrupt");

	if (get_module(NET_NOTIFICATIONS_MODULE_NAME,
			(module_info**)&sNotificationModule) != B_OK) {
		sNotificationModule = NULL;
	}
}


Brcmf::~Brcmf()
{
	Stop();

	for (int32 i = 0; i < fScanCount; i++)
		free(fScanResults[i]);
	while (fRxCount > 0) {
		gBufferModule->free(fRxQueue[fRxHead]);
		fRxHead = (fRxHead + 1) % BRCMF_RX_QUEUE_MAX;
		fRxCount--;
	}

	free(fBounce);
	free(fTxBuffer);
	free(fReply);
	delete_sem(fInterruptSem);
	mutex_destroy(&fBusLock);
	mutex_destroy(&fStateLock);
	mutex_destroy(&fCommandLock);

	if (sNotificationModule != NULL) {
		put_module(NET_NOTIFICATIONS_MODULE_NAME);
		sNotificationModule = NULL;
	}
}


void
Brcmf::SetDeviceName(const char* name)
{
	strlcpy(fDeviceName, name, sizeof(fDeviceName));
}


// #pragma mark - backplane access (bus lock must be held)


uint8
Brcmf::_Read1(uint32 address)
{
	uint8 value = 0xff;
	fHost->ReadDirect((address & ~0x7ffu) == 0 ? 0 : 1, address, &value);
	return value;
}


void
Brcmf::_Write1(uint32 address, uint8 value)
{
	fHost->WriteDirect((address & ~0x7ffu) == 0 ? 0 : 1, address, value);
}


void
Brcmf::_SetWindow(uint32 address)
{
	uint32 window = address & ~kSbWindowMask;
	if (window == fWindow)
		return;

	fHost->WriteDirect(1, F1_SBADDRLOW, (window >> 8) & 0x80);
	fHost->WriteDirect(1, F1_SBADDRMID, (window >> 16) & 0xff);
	fHost->WriteDirect(1, F1_SBADDRHIGH, (window >> 24) & 0xff);
	fWindow = window;
}


uint32
Brcmf::_Read4(uint32 address)
{
	_SetWindow(address);
	uint8 data[4] = { 0xff, 0xff, 0xff, 0xff };
	if (fHost->Extended(false, 1, (address & kSbWindowMask) | kSbAccess32,
			true, data, 4) != B_OK) {
		// force the window to be set again
		fWindow = 0xffffffff;
	}
	return get32(data);
}


void
Brcmf::_Write4(uint32 address, uint32 value)
{
	_SetWindow(address);
	uint8 data[4];
	put32(data, value);
	if (fHost->Extended(true, 1, (address & kSbWindowMask) | kSbAccess32,
			true, data, 4) != B_OK) {
		fWindow = 0xffffffff;
	}
}


status_t
Brcmf::_RamReadWrite(uint32 address, uint8* data, size_t length, bool write)
{
	while (length > 0) {
		_SetWindow(address);
		uint32 offset = address & kSbWindowMask;
		size_t size = min_c(length, (size_t)(kSbWindowMask + 1 - offset));
		size_t rounded = round_up(size, 4);

		status_t status;
		if (write) {
			memcpy(fBounce, data, size);
			memset(fBounce + size, 0, rounded - size);
			status = fHost->Extended(true, 1, offset | kSbAccess32, true,
				fBounce, rounded);
		} else {
			status = fHost->Extended(false, 1, offset | kSbAccess32, true,
				fBounce, rounded);
			memcpy(data, fBounce, size);
		}
		if (status != B_OK) {
			fWindow = 0xffffffff;
			return status;
		}

		address += size;
		data += size;
		length -= size;
	}
	return B_OK;
}


uint32
Brcmf::_DevRead(uint32 reg)
{
	return _Read4(fSdioCoreBase + reg);
}


void
Brcmf::_DevWrite(uint32 reg, uint32 value)
{
	_Write4(fSdioCoreBase + reg, value);
}


// #pragma mark - SDIO card init


status_t
Brcmf::_SdioInit()
{
	status_t status = fHost->Reset();
	if (status != B_OK)
		return status;

	fHost->Command(0, 0, SDIO_RESP_NONE, NULL);
	snooze(2000);

	// IO_SEND_OP_COND: read the OCR, then ask for power up
	uint32 ocr = 0;
	status = B_TIMED_OUT;
	for (int i = 0; i < 10; i++) {
		if (fHost->Command(5, 0, SDIO_RESP_R4, &ocr) == B_OK) {
			status = B_OK;
			break;
		}
		snooze(20000);
	}
	if (status != B_OK) {
		ERROR("no response from the WLAN chip to CMD5\n");
		return status;
	}
	INFO("SDIO OCR %#" B_PRIx32 ", %" B_PRIu32 " functions\n", ocr,
		(ocr >> 28) & 7);

	uint32 voltage = ocr & 0x00ff8000;
	if (voltage == 0)
		voltage = 0x00300000;
	bigtime_t timeout = system_time() + 1000000;
	while (true) {
		if (fHost->Command(5, voltage, SDIO_RESP_R4, &ocr) == B_OK
			&& (ocr & (1u << 31)) != 0) {
			break;
		}
		if (system_time() > timeout) {
			ERROR("WLAN chip did not become ready (OCR %#" B_PRIx32 ")\n",
				ocr);
			return B_TIMED_OUT;
		}
		snooze(10000);
	}

	uint32 response;
	status = fHost->Command(3, 0, SDIO_RESP_R1, &response);
	if (status != B_OK) {
		ERROR("SEND_RELATIVE_ADDR failed\n");
		return status;
	}
	uint32 rca = response >> 16;
	status = fHost->Command(7, rca << 16, SDIO_RESP_R1B, &response);
	if (status != B_OK) {
		ERROR("SELECT_CARD failed\n");
		return status;
	}

	uint8 revision = 0, caps = 0;
	fHost->ReadDirect(0, 0x00, &revision);
	fHost->ReadDirect(0, CCCR_CAPS, &caps);
	TRACE("CCCR revision %#x, capabilities %#x, rca %#" B_PRIx32 "\n",
		revision, caps, rca);

	// 4 bit bus, 25 MHz
	uint8 busif = 0;
	if (fHost->ReadDirect(0, CCCR_BUSIF, &busif) == B_OK
		&& fHost->WriteDirect(0, CCCR_BUSIF, (busif & ~3) | 2) == B_OK) {
		fHost->SetBusWidth(4);
	} else
		ERROR("could not switch to 4 bit bus\n");
	fHost->SetClock(25000000);

	// block sizes
	fHost->WriteDirect(0, 0x100 + FBR_BLOCK_SIZE, 64);
	fHost->WriteDirect(0, 0x100 + FBR_BLOCK_SIZE + 1, 0);
	fHost->WriteDirect(0, 0x200 + FBR_BLOCK_SIZE, 512 & 0xff);
	fHost->WriteDirect(0, 0x200 + FBR_BLOCK_SIZE + 1, 512 >> 8);
	fHost->SetBlockSize(1, 64);
	fHost->SetBlockSize(2, 512);

	// enable function 1
	uint8 ioen = 0;
	fHost->ReadDirect(0, CCCR_IOEN, &ioen);
	fHost->WriteDirect(0, CCCR_IOEN, ioen | (1 << 1));
	fHost->WriteDirect(0, CCCR_INTEN, 0);
	timeout = system_time() + 1000000;
	while (true) {
		uint8 ready = 0;
		if (fHost->ReadDirect(0, CCCR_IORDY, &ready) == B_OK
			&& (ready & (1 << 1)) != 0) {
			break;
		}
		if (system_time() > timeout) {
			ERROR("SDIO function 1 did not become ready\n");
			return B_TIMED_OUT;
		}
		snooze(10000);
	}

	return B_OK;
}


// #pragma mark - chip


brcmf_core*
Brcmf::_Core(uint16 id, int index)
{
	for (int32 i = 0; i < fCoreCount; i++) {
		if (fCores[i].id == id && index-- == 0)
			return &fCores[i];
	}
	return NULL;
}


brcmf_core*
Brcmf::_Pmu()
{
	brcmf_core* cc = _Core(CORE_CHIPCOMMON);
	if (cc != NULL && cc->rev >= 35 && (fCcCapsExt & CAPS_EXT_AOB) != 0) {
		brcmf_core* pmu = _Core(CORE_PMU);
		if (pmu != NULL)
			return pmu;
	}
	return cc;
}


bool
Brcmf::_CoreIsUp(brcmf_core* core)
{
	uint32 ioctl = _Read4(core->wrap + AGENT_IOCTL);
	uint32 reset = _Read4(core->wrap + AGENT_RESET_CTL);
	return (ioctl & (IOCTL_FGC | IOCTL_CLK)) == IOCTL_CLK
		&& (reset & RESET_CTL_RESET) == 0;
}


void
Brcmf::_CoreDisable(brcmf_core* core, uint32 prereset, uint32 reset)
{
	uint32 value = _Read4(core->wrap + AGENT_RESET_CTL);
	if ((value & RESET_CTL_RESET) == 0) {
		_Write4(core->wrap + AGENT_IOCTL, prereset | IOCTL_FGC | IOCTL_CLK);
		_Read4(core->wrap + AGENT_IOCTL);

		_Write4(core->wrap + AGENT_RESET_CTL, RESET_CTL_RESET);
		spin(20);

		int i;
		for (i = 300; i > 0; i--) {
			if (_Read4(core->wrap + AGENT_RESET_CTL) == RESET_CTL_RESET)
				break;
		}
		if (i == 0)
			ERROR("timeout on core %#x reset\n", core->id);
	}

	_Write4(core->wrap + AGENT_IOCTL, reset | IOCTL_FGC | IOCTL_CLK);
	_Read4(core->wrap + AGENT_IOCTL);
}


void
Brcmf::_CoreReset(brcmf_core* core, uint32 prereset, uint32 reset,
	uint32 postreset)
{
	_CoreDisable(core, prereset, reset);

	int i;
	for (i = 50; i > 0; i--) {
		if ((_Read4(core->wrap + AGENT_RESET_CTL) & RESET_CTL_RESET) == 0)
			break;
		_Write4(core->wrap + AGENT_RESET_CTL, 0);
		spin(60);
	}
	if (i == 0)
		ERROR("timeout on core %#x reset release\n", core->id);

	_Write4(core->wrap + AGENT_IOCTL, postreset | IOCTL_CLK);
	_Read4(core->wrap + AGENT_IOCTL);
}


bool
Brcmf::_EromGetRegAddress(uint32& erom, uint32& base, uint32& wrap)
{
	base = 0;
	wrap = 0;

	uint32 value = _Read4(erom);
	uint32 type = value & DMP_DESC_MASK;
	uint32 wrapType;
	if (type == DMP_DESC_MASTER_PORT) {
		wrapType = DMP_SLAVE_TYPE_MWRAP;
		erom += 4;
	} else if ((type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
		wrapType = DMP_SLAVE_TYPE_SWRAP;
	else
		return false;

	int guard = 0;
	do {
		do {
			value = _Read4(erom);
			type = value & DMP_DESC_MASK;
			if (type == DMP_DESC_COMPONENT)
				return true;
			if (type == DMP_DESC_EOT)
				return false;
			erom += 4;
			if (++guard > 512)
				return false;
		} while ((type & ~DMP_DESC_ADDRSIZE_GT32) != DMP_DESC_ADDRESS);

		if ((type & DMP_DESC_ADDRSIZE_GT32) != 0)
			erom += 4;

		uint32 sizeType = (value & DMP_SLAVE_SIZE_TYPE)
			>> DMP_SLAVE_SIZE_TYPE_S;
		if (sizeType == DMP_SLAVE_SIZE_DESC) {
			uint32 size = _Read4(erom);
			if ((size & DMP_DESC_ADDRSIZE_GT32) != 0)
				erom += 8;
			else
				erom += 4;
		}
		if (sizeType != DMP_SLAVE_SIZE_4K && sizeType != DMP_SLAVE_SIZE_8K)
			continue;

		uint32 slaveType = (value & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S;
		if (base == 0 && slaveType == DMP_SLAVE_TYPE_SLAVE)
			base = value & DMP_SLAVE_ADDR_BASE;
		if (wrap == 0 && slaveType == wrapType)
			wrap = value & DMP_SLAVE_ADDR_BASE;
	} while (base == 0 || wrap == 0);

	return true;
}


void
Brcmf::_EromScan()
{
	uint32 erom = _Read4(CHIP_BASE + CHIP_REG_EROMPTR);
	uint32 type = 0;
	int guard = 0;

	while (type != DMP_DESC_EOT && fCoreCount < BRCMF_MAX_CORES
		&& ++guard < 1024) {
		uint32 value = _Read4(erom);
		type = value & DMP_DESC_MASK;
		erom += 4;

		if (type != DMP_DESC_COMPONENT)
			continue;

		uint16 id = (value & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S;

		value = _Read4(erom);
		type = value & DMP_DESC_MASK;
		erom += 4;
		if (type != DMP_DESC_COMPONENT) {
			ERROR("EROM: not a component descriptor\n");
			return;
		}

		uint32 masterWraps = (value & DMP_COMP_NUM_MWRAP)
			>> DMP_COMP_NUM_MWRAP_S;
		uint32 slaveWraps = (value & DMP_COMP_NUM_SWRAP)
			>> DMP_COMP_NUM_SWRAP_S;
		uint8 revision = (value & DMP_COMP_REVISION) >> DMP_COMP_REVISION_S;

		if (masterWraps + slaveWraps == 0 && id != CORE_PMU && id != CORE_GCI)
			continue;

		uint32 base, wrap;
		if (!_EromGetRegAddress(erom, base, wrap))
			continue;

		brcmf_core& core = fCores[fCoreCount++];
		core.id = id;
		core.rev = revision;
		core.base = base;
		core.wrap = wrap;
	}
}


void
Brcmf::_SetPassive()
{
	brcmf_core* core = _Core(CORE_ARM_CR4);
	if (core == NULL)
		core = _Core(CORE_ARM_CA7);
	if (core != NULL) {
		uint32 value = _Read4(core->wrap + AGENT_IOCTL);
		_CoreReset(core, value & IOCTL_CR4_CPUHALT, IOCTL_CR4_CPUHALT,
			IOCTL_CR4_CPUHALT);

		brcmf_core* d11;
		for (int i = 0; (d11 = _Core(CORE_80211, i)) != NULL; i++) {
			_CoreDisable(d11, IOCTL_D11_PHYRESET | IOCTL_D11_PHYCLOCKEN,
				IOCTL_D11_PHYCLOCKEN);
		}
		return;
	}

	core = _Core(CORE_ARM_CM3);
	if (core != NULL) {
		_CoreDisable(core, 0, 0);
		brcmf_core* d11 = _Core(CORE_80211);
		if (d11 != NULL) {
			_CoreReset(d11, IOCTL_D11_PHYRESET | IOCTL_D11_PHYCLOCKEN,
				IOCTL_D11_PHYCLOCKEN, IOCTL_D11_PHYCLOCKEN);
		}
		brcmf_core* ram = _Core(CORE_INTERNAL_MEM);
		if (ram != NULL) {
			_CoreReset(ram, 0, 0, 0);
			if (fChip == 43430) {
				_Write4(ram->base + SOCRAM_BANKIDX, 3);
				_Write4(ram->base + SOCRAM_BANKPDA, 0);
			}
		}
	}
}


status_t
Brcmf::_SetActive(uint32 resetVector)
{
	brcmf_core* core = _Core(CORE_ARM_CR4);
	if (core == NULL)
		core = _Core(CORE_ARM_CA7);
	if (core != NULL) {
		_DevWrite(SD_INTSTATUS, 0xffffffff);
		if (resetVector != 0) {
			uint8 vector[4];
			put32(vector, resetVector);
			_RamReadWrite(0, vector, 4, true);
		}
		_CoreReset(core, IOCTL_CR4_CPUHALT, 0, 0);
		return B_OK;
	}

	core = _Core(CORE_ARM_CM3);
	if (core != NULL) {
		brcmf_core* ram = _Core(CORE_INTERNAL_MEM);
		if (ram == NULL || !_CoreIsUp(ram))
			return B_ERROR;
		_DevWrite(SD_INTSTATUS, 0xffffffff);
		_CoreReset(core, 0, 0, 0);
		return B_OK;
	}

	return B_ERROR;
}


status_t
Brcmf::_ChipAttach()
{
	// Request the ALP clock, needed for backplane access.
	uint8 clockSet = CLK_ALP_AVAIL_REQ | CLK_FORCE_HW_CLKREQ_OFF;
	_Write1(F1_CHIPCLKCSR, clockSet);
	uint8 clock = _Read1(F1_CHIPCLKCSR);
	if ((clock & ~(CLK_ALP_AVAIL | CLK_HT_AVAIL)) != clockSet) {
		ERROR("ChipClkCSR: wrote %#x, read %#x\n", clockSet, clock);
		return B_ERROR;
	}
	bigtime_t timeout = system_time() + 100000;
	while ((clock & (CLK_ALP_AVAIL | CLK_HT_AVAIL)) == 0) {
		if (system_time() > timeout) {
			ERROR("timeout waiting for the ALP clock (%#x)\n", clock);
			return B_TIMED_OUT;
		}
		spin(100);
		clock = _Read1(F1_CHIPCLKCSR);
	}
	_Write1(F1_CHIPCLKCSR, CLK_FORCE_HW_CLKREQ_OFF | CLK_FORCE_ALP);
	spin(65);
	_Write1(F1_SDIOPULLUP, 0);

	uint32 id = _Read4(CHIP_BASE + CHIP_REG_CHIPID);
	fChip = id & 0xffff;
	fChipRev = (id >> 16) & 0xf;
	uint32 interconnect = (id >> 28) & 0xf;
	INFO("chip %s%x (%u) revision %u, interconnect %" B_PRIu32 "\n",
		fChip > 0xa000 ? "" : "BCM", fChip, fChip, fChipRev, interconnect);
	if (interconnect != 1) {
		ERROR("unsupported interconnect type %" B_PRIu32 "\n", interconnect);
		return B_NOT_SUPPORTED;
	}

	_EromScan();

	bool cpuFound = false;
	for (int32 i = 0; i < fCoreCount; i++) {
		TRACE("core %#x rev %u base %#" B_PRIx32 " wrap %#" B_PRIx32 "\n",
			fCores[i].id, fCores[i].rev, fCores[i].base, fCores[i].wrap);
		switch (fCores[i].id) {
			case CORE_ARM_CM3:
			case CORE_ARM_CR4:
			case CORE_ARM_CA7:
				cpuFound = true;
				break;
		}
	}
	INFO("%" B_PRId32 " cores found\n", fCoreCount);
	if (!cpuFound) {
		ERROR("CPU core not found\n");
		return B_ERROR;
	}

	brcmf_core* sdio = _Core(CORE_SDIO_DEV);
	brcmf_core* cc = _Core(CORE_CHIPCOMMON);
	if (sdio == NULL || cc == NULL) {
		ERROR("SDIO or chipcommon core not found\n");
		return B_ERROR;
	}
	fSdioCoreBase = sdio->base;

	_SetPassive();

	// RAM size
	brcmf_core* core;
	if ((core = _Core(CORE_ARM_CR4)) != NULL) {
		uint32 cap = _Read4(core->base + CR4_CAP);
		uint32 banks = (cap & 0xf) + ((cap >> 4) & 0xf);
		uint32 size = 0;
		for (uint32 i = 0; i < banks; i++) {
			_Write4(core->base + CR4_BANKIDX, i);
			uint32 info = _Read4(core->base + CR4_BANKINFO);
			uint32 blockSize = (info & 0x200) != 0 ? 1024 : 8192;
			size += ((info & 0x7f) + 1) * blockSize;
		}
		fRamSize = size;
		switch (fChip) {
			case 0x4345:
				fRamBase = 0x198000;
				break;
			case 0x4335:
			case 0x4339:
			case 0x4350:
			case 0x4354:
			case 0x4356:
				fRamBase = 0x180000;
				break;
			default:
				ERROR("unknown RAM base for chip %#x\n", fChip);
				return B_NOT_SUPPORTED;
		}
	} else if ((core = _Core(CORE_INTERNAL_MEM)) != NULL) {
		if (!_CoreIsUp(core))
			_CoreReset(core, 0, 0, 0);
		uint32 info = _Read4(core->base + SOCRAM_COREINFO);
		uint32 banks = (info >> 4) & 0xf;
		uint32 size = 0;
		if (core->rev <= 7 || core->rev == 12) {
			ERROR("unsupported SOCRAM revision %u\n", core->rev);
			return B_NOT_SUPPORTED;
		}
		for (uint32 i = 0; i < banks; i++) {
			_Write4(core->base + SOCRAM_BANKIDX, i);
			uint32 bank = _Read4(core->base + SOCRAM_BANKINFO);
			size += ((bank & 0x7f) + 1) * 8192;
		}
		fRamSize = size;
		fRamBase = 0;
	} else {
		ERROR("no RAM core\n");
		return B_ERROR;
	}
	INFO("RAM %" B_PRIu32 " KB at %#" B_PRIx32 "\n", fRamSize / 1024,
		fRamBase);

	fCcCaps = _Read4(cc->base + CHIP_REG_CAPS);
	fCcCapsExt = _Read4(cc->base + CHIP_REG_CAPS_EXT);

	return B_OK;
}


// #pragma mark - probe / start


status_t
Brcmf::Probe()
{
	if (fBounce == NULL || fTxBuffer == NULL || fReply == NULL
		|| fInterruptSem < 0) {
		return B_NO_MEMORY;
	}

	MutexLocker locker(fBusLock);

	status_t status = _SdioInit();
	if (status != B_OK)
		return status;

	// Force the PLL off until we need it
	_Write1(F1_CHIPCLKCSR, CLK_FORCE_HW_CLKREQ_OFF | CLK_ALP_AVAIL_REQ);

	status = _ChipAttach();
	if (status != B_OK)
		return status;

	brcmf_core* sdio = _Core(CORE_SDIO_DEV);
	if (sdio->rev >= 12) {
		uint8 value = _Read1(F1_SLEEPCSR);
		if ((value & SLEEPCSR_KSO) == 0)
			_Write1(F1_SLEEPCSR, value | SLEEPCSR_KSO);
	}

	_Write1(CCCR_BRCM_CARDCTRL,
		_Read1(CCCR_BRCM_CARDCTRL) | CARDCTRL_WLANRESET);

	brcmf_core* pmu = _Pmu();
	if (pmu != NULL) {
		_Write4(pmu->base + CHIP_REG_PMUCONTROL,
			_Read4(pmu->base + CHIP_REG_PMUCONTROL) | (0x2 << 13));
	}

	// disable function 2 until the firmware runs
	uint8 ioen = _Read1(CCCR_IOEN);
	_Write1(CCCR_IOEN, ioen & ~(1 << 2));

	_Write1(F1_CHIPCLKCSR, 0);

	fProbed = true;
	return B_OK;
}


status_t
Brcmf::_WaitClock(bool alpOnly)
{
	_Write1(F1_CHIPCLKCSR, alpOnly ? CLK_ALP_AVAIL_REQ : CLK_HT_AVAIL_REQ);

	bigtime_t timeout = system_time() + 5000000;
	while (true) {
		uint8 clock = _Read1(F1_CHIPCLKCSR);
		uint8 available = clock & (CLK_ALP_AVAIL | CLK_HT_AVAIL);
		if (alpOnly ? available != 0
				: available == (CLK_ALP_AVAIL | CLK_HT_AVAIL)) {
			return B_OK;
		}
		if (system_time() > timeout) {
			ERROR("timeout waiting for the %s clock (%#x)\n",
				alpOnly ? "ALP" : "HT", clock);
			return B_TIMED_OUT;
		}
		snooze(1000);
	}
}


status_t
Brcmf::_ReadFile(const char* name, uint8** _data, size_t* _size)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY,
	};
	static const char* kSubdirectories[] = { "brcm", "brcmfmac" };

	char path[B_PATH_NAME_LENGTH];
	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories) && fd < 0; i++) {
		char base[B_PATH_NAME_LENGTH];
		if (find_directory(kDirectories[i], -1, false, base, sizeof(base))
				!= B_OK) {
			continue;
		}
		for (size_t j = 0; j < B_COUNT_OF(kSubdirectories) && fd < 0; j++) {
			snprintf(path, sizeof(path), "%s/firmware/%s/%s", base,
				kSubdirectories[j], name);
			fd = open(path, O_RDONLY);
		}
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;

	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if (size <= 0 || size > 4 * 1024 * 1024) {
		close(fd);
		return B_BAD_DATA;
	}

	uint8* data = (uint8*)malloc(size + 64);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		free(data);
		return B_IO_ERROR;
	}

	TRACE("loaded %s (%" B_PRIdOFF " bytes)\n", path, size);
	*_data = data;
	*_size = size;
	return B_OK;
}


/*!	Converts the textual NVRAM file to what the firmware wants: NUL separated
	"key=value" entries without comments, padded to 4 bytes, followed by a
	length token.
*/
status_t
Brcmf::_ConvertNvram(uint8* data, size_t size, uint8** _nvram,
	size_t* _length)
{
	uint8* nvram = (uint8*)malloc(size + 64);
	if (nvram == NULL)
		return B_NO_MEMORY;

	uint8* dst = nvram;
	size_t count = 0;
	bool skip = false;
	for (size_t i = 0; i < size; i++) {
		uint8 c = data[i];
		if (c == '\n') {
			if (count > 0)
				*dst++ = '\0';
			count = 0;
			skip = false;
			continue;
		}
		if (skip)
			continue;
		if (c == '#' && count == 0) {
			skip = true;
			continue;
		}
		if (c == '\r')
			continue;
		*dst++ = c;
		count++;
	}
	if (count > 0)
		*dst++ = '\0';

	size_t length = dst - nvram;
	size_t padding = round_up(length + 1, 4) - length;
	memset(dst, 0, padding);
	length += padding;
	dst += padding;

	uint32 token = (length / 4) & 0xffff;
	token |= ~token << 16;
	put32(dst, token);
	length += 4;

	*_nvram = nvram;
	*_length = length;
	return B_OK;
}


status_t
Brcmf::_LoadFirmware()
{
	switch (fChip) {
		case 0x4345:
			strlcpy(fFirmwareBase, fChipRev == 9 ? "brcmfmac43456-sdio"
				: "brcmfmac43455-sdio", sizeof(fFirmwareBase));
			break;
		case 43430:
			strlcpy(fFirmwareBase, fChipRev == 2 ? "brcmfmac43436-sdio"
				: "brcmfmac43430-sdio", sizeof(fFirmwareBase));
			break;
		default:
			ERROR("no firmware known for chip %#x\n", fChip);
			return B_NOT_SUPPORTED;
	}

	char name[64];
	uint8* firmware;
	size_t firmwareSize;
	snprintf(name, sizeof(name), "%s.bin", fFirmwareBase);
	status_t status = _ReadFile(name, &firmware, &firmwareSize);
	if (status != B_OK) {
		ERROR("cannot load firmware %s: %s\n", name, strerror(status));
		return status;
	}

	uint8* text;
	size_t textSize;
	snprintf(name, sizeof(name), "%s.txt", fFirmwareBase);
	status = _ReadFile(name, &text, &textSize);
	if (status != B_OK) {
		ERROR("cannot load NVRAM %s: %s\n", name, strerror(status));
		free(firmware);
		return status;
	}

	uint8* nvram;
	size_t nvramLength;
	status = _ConvertNvram(text, textSize, &nvram, &nvramLength);
	free(text);
	if (status != B_OK) {
		free(firmware);
		return status;
	}

	INFO("loading firmware %s (%" B_PRIuSIZE " bytes, NVRAM %" B_PRIuSIZE
		" bytes)\n", fFirmwareBase, firmwareSize, nvramLength);

	status = _WaitClock(true);
	uint8* verify = NULL;
	bigtime_t start = system_time();
	if (status == B_OK) {
		status = _RamReadWrite(fRamBase, firmware, firmwareSize, true);
		if (status != B_OK)
			ERROR("firmware upload failed: %s\n", strerror(status));
	}
	if (status == B_OK) {
		verify = (uint8*)malloc(firmwareSize);
		if (verify == NULL)
			status = B_NO_MEMORY;
	}
	if (status == B_OK) {
		status = _RamReadWrite(fRamBase, verify, firmwareSize, false);
		if (status == B_OK && memcmp(verify, firmware, firmwareSize) != 0) {
			size_t offset = 0;
			while (offset < firmwareSize && verify[offset] == firmware[offset])
				offset++;
			ERROR("firmware verification failed at offset %#" B_PRIxSIZE
				"\n", offset);
			status = B_IO_ERROR;
		}
	}
	if (status == B_OK) {
		uint32 address = fRamBase + fRamSize - nvramLength;
		status = _RamReadWrite(address, nvram, nvramLength, true);
		if (status == B_OK) {
			status = _RamReadWrite(address, verify, nvramLength, false);
			if (status == B_OK && memcmp(verify, nvram, nvramLength) != 0) {
				ERROR("NVRAM verification failed\n");
				status = B_IO_ERROR;
			}
		}
	}
	if (status == B_OK) {
		INFO("firmware uploaded in %" B_PRId64 " ms\n",
			(system_time() - start) / 1000);
		status = _SetActive(get32(firmware));
	}

	free(verify);
	free(firmware);
	free(nvram);

	_Write1(F1_CHIPCLKCSR, 0);
	return status;
}


status_t
Brcmf::Start()
{
	if (fRunning)
		return B_OK;
	if (!fProbed)
		return B_NO_INIT;

	{
		MutexLocker locker(fBusLock);

		status_t status = _LoadFirmware();
		if (status != B_OK)
			return status;

		status = _WaitClock(false);
		if (status != B_OK) {
			ERROR("firmware did not start (no HT clock)\n");
			return status;
		}

		uint8 savedClock = _Read1(F1_CHIPCLKCSR);
		_Write1(F1_CHIPCLKCSR, savedClock | CLK_FORCE_HT);

		_DevWrite(SD_TOSBMAILBOXDATA, kSdpcmProtocolVersion << 16);

		// enable function 2 (frames)
		uint8 ioen = _Read1(CCCR_IOEN);
		_Write1(CCCR_IOEN, ioen | (1 << 2));
		bigtime_t timeout = system_time() + 3000000;
		while (true) {
			uint8 ready = _Read1(CCCR_IORDY);
			if ((ready & (1 << 2)) != 0)
				break;
			if (system_time() > timeout) {
				ERROR("SDIO function 2 did not become ready (%#x)\n", ready);
				return B_TIMED_OUT;
			}
			snooze(10000);
		}

		_DevWrite(SD_HOSTINTMASK, INTSTATUS_HMB_SW_MASK
			| INTSTATUS_CHIPACTIVE);
		_Write1(F1_WATERMARK, 8);
		_Write1(F1_CHIPCLKCSR, savedClock);

		// SDIO interrupts: master, function 1 and 2
		_Write1(CCCR_INTEN, (1 << 0) | (1 << 1) | (1 << 2));
	}

	fStopReader = false;
	fReaderThread = spawn_kernel_thread(_ReaderThread, "brcmfmac reader",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fReaderThread < 0)
		return fReaderThread;
	resume_thread(fReaderThread);

	fHost->EnableCardInterrupt(true);

	status_t status = _LoadClm();
	if (status != B_OK)
		INFO("no CLM data loaded, channels may be limited\n");

	status = _InitFirmware();
	if (status != B_OK) {
		ERROR("firmware initialization failed: %s\n", strerror(status));
		Stop();
		return status;
	}

	fRunning = true;
	return B_OK;
}


void
Brcmf::Stop()
{
	if (fReaderThread >= 0) {
		fStopReader = true;
		release_sem(fInterruptSem);
		status_t result;
		wait_for_thread(fReaderThread, &result);
		fReaderThread = -1;
	}
	fHost->EnableCardInterrupt(false);
	fRunning = false;
}


// #pragma mark - frames


status_t
Brcmf::_FrameReadWrite(uint8* buffer, size_t length, bool write)
{
	_SetWindow(CHIP_BASE);
	uint32 address = (CHIP_BASE & kSbWindowMask) | kSbAccess32;
	status_t status = fHost->Extended(write, 2, address, write, buffer,
		length);
	if (status != B_OK) {
		// abort the frame
		fHost->WriteDirect(0, CCCR_IOABORT, 2);
		fHost->WriteDirect(1, F1_FRAMECTL,
			write ? FRAMECTL_WF_TERM : FRAMECTL_RF_TERM);
		uint32 counter = write ? F1_WFRMCNT : F1_RFRMCNT;
		for (int i = 0; i < 100; i++) {
			uint8 high = 0xff, low = 0xff;
			fHost->ReadDirect(1, counter + 1, &high);
			fHost->ReadDirect(1, counter, &low);
			if (high == 0 && low == 0)
				break;
		}
	}
	return status;
}


bool
Brcmf::_TxWindowOpen() const
{
	uint8 room = fTxMax - fTxSeq;
	return room != 0 && (room & 0x80) == 0;
}


/*!	Sends a frame on the given channel. The payload consists of \a header
	followed by either \a buffer (if not NULL) or \a data.
	The bus lock must be held.
*/
status_t
Brcmf::_SendFrame(uint8 channel, const uint8* header, size_t headerLength,
	net_buffer* buffer, const uint8* data, size_t length)
{
	size_t total = kSdpcmHeaderSize + headerLength + length;
	size_t padded = total > 512 && (total % 512) != 0
		? round_up(total, 512) : round_up(total, 4);
	if (padded > BRCMF_BUFFER_SIZE)
		return B_BAD_VALUE;

	uint8* frame = fTxBuffer;
	put16(frame, total);
	put16(frame + 2, ~total);
	frame[4] = fTxSeq;
	frame[5] = channel;
	frame[6] = 0;					// next length
	frame[7] = kSdpcmHeaderSize;	// data offset
	frame[8] = 0;					// flow control
	frame[9] = 0;					// max sequence
	frame[10] = 0;
	frame[11] = 0;

	if (headerLength > 0)
		memcpy(frame + kSdpcmHeaderSize, header, headerLength);
	uint8* payload = frame + kSdpcmHeaderSize + headerLength;
	if (buffer != NULL) {
		if (gBufferModule->read(buffer, 0, payload, length) != B_OK)
			return B_BAD_DATA;
	} else if (length > 0)
		memcpy(payload, data, length);
	memset(frame + total, 0, padded - total);

	status_t status = _FrameReadWrite(frame, padded, true);
	if (status == B_OK)
		fTxSeq++;
	return status;
}


status_t
Brcmf::_ReaderThread(void* self)
{
	((Brcmf*)self)->_ReaderLoop();
	return B_OK;
}


void
Brcmf::InterruptReceived()
{
	release_sem_etc(fInterruptSem, 1, B_DO_NOT_RESCHEDULE);
}


void
Brcmf::_ReaderLoop()
{
	while (!fStopReader) {
		acquire_sem_etc(fInterruptSem, 1, B_RELATIVE_TIMEOUT, 50000);
		if (fStopReader)
			break;

		{
			MutexLocker locker(fBusLock);
			_ServiceInterrupts();
		}

		fHost->EnableCardInterrupt(true);
	}
}


void
Brcmf::_ServiceInterrupts()
{
	uint8 pending = 0;
	if (fHost->ReadDirect(0, CCCR_INTPEND, &pending) != B_OK)
		return;

	uint32 status = 0;
	if (pending != 0) {
		status = _DevRead(SD_INTSTATUS);
		uint32 handled = status & (INTSTATUS_HMB_SW_MASK
			| INTSTATUS_CHIPACTIVE);
		if (handled != 0)
			_DevWrite(SD_INTSTATUS, handled);

		if ((status & INTSTATUS_HMB_HOST_INT) != 0) {
			uint32 hostData = _DevRead(SD_TOHOSTMAILBOXDATA);
			_DevWrite(SD_TOSBMAILBOX, TOSBMAILBOX_INT_ACK);
			if ((hostData & HOSTMB_NAKHANDLED) != 0)
				status |= INTSTATUS_HMB_FRAME_IND;
			if ((hostData & HOSTMB_FWREADY) != 0)
				TRACE("firmware ready\n");
			if ((hostData & HOSTMB_FWHALT) != 0)
				ERROR("firmware halted!\n");
		}
	}

	if (pending != 0 || (status & INTSTATUS_HMB_FRAME_IND) != 0)
		_ReadFrames();
}


void
Brcmf::_ReadFrames()
{
	uint8* frame = fBounce;

	for (int frames = 0; frames < 128; frames++) {
		if (_FrameReadWrite(frame, kSdpcmHeaderSize, false) != B_OK)
			break;

		uint16 length = get16(frame);
		uint16 check = get16(frame + 2);
		if (length == 0 && check == 0)
			break;
		if ((length ^ check) != 0xffff || length < kSdpcmHeaderSize
			|| length > BRCMF_BUFFER_SIZE - 4) {
			ERROR("bad frame header (length %#x check %#x)\n", length, check);
			fHost->WriteDirect(0, CCCR_IOABORT, 2);
			fHost->WriteDirect(1, F1_FRAMECTL, FRAMECTL_RF_TERM);
			break;
		}

		uint8 sequenceMax = frame[9];
		if (sequenceMax != fTxMax) {
			fTxMax = sequenceMax;
			fTxCondition.NotifyAll();
		}
		fFlowControl = frame[8];

		if (length > kSdpcmHeaderSize) {
			if (_FrameReadWrite(frame + kSdpcmHeaderSize,
					round_up(length - kSdpcmHeaderSize, 4), false) != B_OK) {
				break;
			}
		}

		uint8 channel = frame[5] & 0x0f;
		uint8 dataOffset = frame[7];
		if (dataOffset < kSdpcmHeaderSize || dataOffset > length)
			continue;
		uint8* data = frame + dataOffset;
		size_t dataLength = length - dataOffset;

		switch (channel) {
			case CHANNEL_CONTROL:
				_HandleControl(data, dataLength);
				break;
			case CHANNEL_EVENT:
			case CHANNEL_DATA:
			{
				if (dataLength < kBcdcHeaderSize)
					break;
				size_t offset = kBcdcHeaderSize + data[3] * 4;
				if (offset > dataLength)
					break;
				if (channel == CHANNEL_EVENT)
					_HandleEvent(data + offset, dataLength - offset);
				else
					_HandleData(data + offset, dataLength - offset);
				break;
			}
			case CHANNEL_GLOM:
				ERROR("unexpected superframe, dropped\n");
				break;
			default:
				ERROR("frame on unknown channel %u\n", channel);
				break;
		}
	}
}


void
Brcmf::_HandleControl(uint8* data, size_t length)
{
	if (length < kDcmdHeaderSize)
		return;

	uint32 flags = get32(data + 8);
	uint16 id = flags >> 16;
	if (id != fPendingId || fReplyDone)
		return;

	size_t copy = min_c(length, kDcmdHeaderSize + kMaxDcmdPayload);
	memcpy(fReply, data, copy);
	fReplyLength = copy;
	fReplyDone = true;
	fReplyCondition.NotifyAll();
}


void
Brcmf::_HandleData(uint8* data, size_t length)
{
	if (length < 14)
		return;

	net_buffer* buffer = gBufferModule->create(0);
	if (buffer == NULL)
		return;
	if (gBufferModule->append(buffer, data, length) != B_OK) {
		gBufferModule->free(buffer);
		return;
	}

	cpu_status state = disable_interrupts();
	acquire_spinlock(&fRxLock);
	bool queued = false;
	if (fRxCount < BRCMF_RX_QUEUE_MAX) {
		fRxQueue[(fRxHead + fRxCount) % BRCMF_RX_QUEUE_MAX] = buffer;
		fRxCount++;
		queued = true;
	}
	release_spinlock(&fRxLock);
	restore_interrupts(state);

	if (queued)
		fRxCondition.NotifyAll();
	else
		gBufferModule->free(buffer);
}


status_t
Brcmf::Receive(net_buffer** _buffer, bool block)
{
	while (true) {
		ConditionVariableEntry entry;
		fRxCondition.Add(&entry);

		cpu_status state = disable_interrupts();
		acquire_spinlock(&fRxLock);
		net_buffer* buffer = NULL;
		if (fRxCount > 0) {
			buffer = fRxQueue[fRxHead];
			fRxHead = (fRxHead + 1) % BRCMF_RX_QUEUE_MAX;
			fRxCount--;
		}
		release_spinlock(&fRxLock);
		restore_interrupts(state);

		if (buffer != NULL) {
			*_buffer = buffer;
			return B_OK;
		}
		if (!block)
			return B_WOULD_BLOCK;

		status_t status = entry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
			1000000);
		if (status != B_OK && status != B_TIMED_OUT)
			return status;
	}
}


status_t
Brcmf::Send(net_buffer* buffer)
{
	if (!fRunning)
		return B_DEV_NOT_READY;
	if (buffer->size > 1600)
		return B_BAD_VALUE;

	// wait for the firmware to accept more frames
	bigtime_t timeout = system_time() + 500000;
	while (!_TxWindowOpen()) {
		if (system_time() > timeout) {
			TRACE("tx window closed (seq %u max %u), dropping frame\n",
				fTxSeq, fTxMax);
			return B_BUSY;
		}
		ConditionVariableEntry entry;
		fTxCondition.Add(&entry);
		if (_TxWindowOpen())
			break;
		entry.Wait(B_RELATIVE_TIMEOUT, 20000);
	}

	uint8 bcdc[kBcdcHeaderSize] = { 0x20, 0, 0, 0 };
		// protocol version 2, priority 0, interface 0, no extra offset

	MutexLocker locker(fBusLock);
	status_t status = _SendFrame(CHANNEL_DATA, bcdc, sizeof(bcdc), buffer,
		NULL, buffer->size);
	locker.Unlock();

	if (status == B_OK)
		gBufferModule->free(buffer);
	return status;
}


// #pragma mark - control


status_t
Brcmf::_Dcmd(uint32 command, bool set, void* data, size_t length,
	size_t* _replyLength)
{
	if (length > kMaxDcmdPayload)
		return B_BAD_VALUE;
	if (fReaderThread < 0)
		return B_NO_INIT;

	MutexLocker commandLocker(fCommandLock);

	uint16 id = ++fRequestId;
	uint8* request = (uint8*)malloc(kDcmdHeaderSize + length);
	if (request == NULL)
		return B_NO_MEMORY;
	put32(request, command);
	put32(request + 4, length);
	put32(request + 8, (set ? DCMD_SET : 0) | ((uint32)id << 16));
	put32(request + 12, 0);
	if (length > 0)
		memcpy(request + kDcmdHeaderSize, data, length);

	ConditionVariableEntry entry;
	fReplyCondition.Add(&entry);
	fReplyDone = false;
	fPendingId = id;

	{
		MutexLocker locker(fBusLock);
		status_t status = _SendFrame(CHANNEL_CONTROL, NULL, 0, NULL, request,
			kDcmdHeaderSize + length);
		free(request);
		if (status != B_OK) {
			fPendingId = 0;
			return status;
		}
	}

	// the reader thread might not see an interrupt for a moment
	release_sem(fInterruptSem);

	bigtime_t timeout = system_time() + 2000000;
	while (!fReplyDone) {
		status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT, timeout);
		if (fReplyDone)
			break;
		if (status == B_TIMED_OUT) {
			ERROR("no reply to command %" B_PRIu32 "\n", command);
			fPendingId = 0;
			return B_TIMED_OUT;
		}
		fReplyCondition.Add(&entry);
	}
	fPendingId = 0;

	uint32 flags = get32(fReply + 8);
	if ((flags & DCMD_ERROR) != 0) {
		TRACE("command %" B_PRIu32 " failed: firmware status %" B_PRId32 "\n",
			command, (int32)get32(fReply + 12));
		return B_ERROR;
	}

	size_t replyLength = fReplyLength > kDcmdHeaderSize
		? fReplyLength - kDcmdHeaderSize : 0;
	if (!set && data != NULL)
		memcpy(data, fReply + kDcmdHeaderSize, min_c(length, replyLength));
	if (_replyLength != NULL)
		*_replyLength = replyLength;
	return B_OK;
}


status_t
Brcmf::_GetVar(const char* name, void* data, size_t length)
{
	size_t nameLength = strlen(name) + 1;
	size_t total = max_c(nameLength, length);
	uint8* buffer = (uint8*)calloc(1, total);
	if (buffer == NULL)
		return B_NO_MEMORY;
	memcpy(buffer, name, nameLength);

	status_t status = _Dcmd(C_GET_VAR, false, buffer, total, NULL);
	if (status == B_OK)
		memcpy(data, buffer, length);
	free(buffer);
	return status;
}


status_t
Brcmf::_SetVar(const char* name, const void* data, size_t length)
{
	size_t nameLength = strlen(name) + 1;
	uint8* buffer = (uint8*)malloc(nameLength + length);
	if (buffer == NULL)
		return B_NO_MEMORY;
	memcpy(buffer, name, nameLength);
	if (length > 0)
		memcpy(buffer + nameLength, data, length);

	status_t status = _Dcmd(C_SET_VAR, true, buffer, nameLength + length,
		NULL);
	free(buffer);
	if (status != B_OK)
		TRACE("setting \"%s\" failed\n", name);
	return status;
}


status_t
Brcmf::_GetVarInt(const char* name, uint32* value)
{
	uint8 data[4] = {};
	status_t status = _GetVar(name, data, 4);
	if (status == B_OK)
		*value = get32(data);
	return status;
}


status_t
Brcmf::_SetVarInt(const char* name, uint32 value)
{
	uint8 data[4];
	put32(data, value);
	return _SetVar(name, data, 4);
}


status_t
Brcmf::_CmdInt(uint32 command, uint32 value)
{
	uint8 data[4];
	put32(data, value);
	return _Dcmd(command, true, data, 4, NULL);
}


status_t
Brcmf::_LoadClm()
{
	char name[64];
	snprintf(name, sizeof(name), "%s.clm_blob", fFirmwareBase);
	uint8* clm;
	size_t size;
	status_t status = _ReadFile(name, &clm, &size);
	if (status != B_OK)
		return status;

	const size_t kChunk = 1400;
	const size_t kHeader = 12;
	uint8* chunk = (uint8*)malloc(kHeader + kChunk);
	if (chunk == NULL) {
		free(clm);
		return B_NO_MEMORY;
	}

	size_t offset = 0;
	while (offset < size) {
		size_t length = min_c(size - offset, kChunk);
		uint16 flags = 1 << 12;			// handler version 1
		if (offset == 0)
			flags |= 1 << 1;			// begin
		if (offset + length == size)
			flags |= 1 << 2;			// end
		put16(chunk, flags);
		put16(chunk + 2, 2);			// type: CLM
		put32(chunk + 4, length);
		put32(chunk + 8, 0);			// CRC
		memcpy(chunk + kHeader, clm + offset, length);

		status = _SetVar("clmload", chunk, kHeader + length);
		if (status != B_OK)
			break;
		offset += length;
	}
	free(chunk);
	free(clm);

	uint32 clmStatus = 0;
	if (_GetVarInt("clmload_status", &clmStatus) == B_OK && clmStatus != 0)
		ERROR("CLM load status %" B_PRIu32 "\n", clmStatus);
	else if (status == B_OK)
		TRACE("CLM data loaded\n");
	return status;
}


status_t
Brcmf::_InitFirmware()
{
	status_t status = _GetVar("cur_etheraddr", fMacAddress, 6);
	if (status != B_OK) {
		ERROR("cannot get MAC address\n");
		return status;
	}
	INFO("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", fMacAddress[0],
		fMacAddress[1], fMacAddress[2], fMacAddress[3], fMacAddress[4],
		fMacAddress[5]);

	char text[256];
	memset(text, 0, sizeof(text));
	if (_GetVar("ver", text, sizeof(text) - 1) == B_OK) {
		char* newline = strchr(text, '\n');
		if (newline != NULL)
			*newline = '\0';
		INFO("firmware: %s\n", text);
	}
	memset(text, 0, sizeof(text));
	if (_GetVar("cap", text, sizeof(text) - 1) == B_OK) {
		char* newline = strchr(text, '\n');
		if (newline != NULL)
			*newline = '\0';
		INFO("firmware capabilities: %s\n", text);
		fHasFirmwareSupplicant = strstr(text, "idsup") != NULL;
	}
	if (_GetVarInt("scan_ver", &fScanVersion) != B_OK)
		fScanVersion = 0;

	_SetVarInt("assoc_listen", 10);
	_CmdInt(C_SET_PM, 0);
		// power save off
	_SetVarInt("bus:txglom", 0);
	_SetVarInt("bcn_timeout", 10);
	_SetVarInt("assoc_retry_max", 3);

	uint8 eventMask[16];
	memset(eventMask, 0xff, sizeof(eventMask));
	static const int kDisabledEvents[] = { 20, 40, 44, 54, 71, 124 };
	for (size_t i = 0; i < B_COUNT_OF(kDisabledEvents); i++) {
		int event = kDisabledEvents[i];
		eventMask[event / 8] &= ~(1 << (event % 8));
	}
	status = _SetVar("event_msgs", eventMask, sizeof(eventMask));
	if (status != B_OK)
		ERROR("cannot set event mask\n");

	_CmdInt(C_SET_SCAN_CHANNEL_TIME, 40);
	_CmdInt(C_SET_SCAN_UNASSOC_TIME, 40);
	_CmdInt(C_SET_SCAN_PASSIVE_TIME, 130);
	_SetVarInt("roam_off", 1);
	_CmdInt(C_SET_INFRA, 1);
	_CmdInt(C_SET_PROMISC, 0);
	_SetVarInt("allmulti", 1);
	_SetVarInt("arpoe", 0);
	_SetVarInt("ndoe", 0);

	status = _CmdInt(C_UP, 1);
	if (status != B_OK) {
		ERROR("cannot bring the interface up\n");
		return status;
	}
	return B_OK;
}


void
Brcmf::SetPromiscuous(bool enable)
{
	if (fRunning)
		_CmdInt(C_SET_PROMISC, enable ? 1 : 0);
}


// #pragma mark - events


void
Brcmf::_Notify(int32 opcode)
{
	if (sNotificationModule == NULL)
		return;

	char messageBuffer[512];
	KMessage message;
	message.SetTo(messageBuffer, sizeof(messageBuffer), B_NETWORK_MONITOR);
	message.AddInt32("opcode", opcode);
	message.AddString("interface", fDeviceName);
	sNotificationModule->send_notification(&message);
}


void
Brcmf::_SetLink(bool up)
{
	if (fLinkUp == up)
		return;
	fLinkUp = up;
	INFO("link %s\n", up ? "up" : "down");

	if (fLinkStateSem >= 0)
		release_sem_etc(fLinkStateSem, 1, B_DO_NOT_RESCHEDULE);
	_Notify(up ? B_NETWORK_WLAN_JOINED : B_NETWORK_WLAN_LEFT);
}


void
Brcmf::_HandleEvent(uint8* data, size_t length)
{
	// Ethernet header (14) + Broadcom header (10) + event message (48)
	if (length < 72)
		return;
	if (get16be(data + 12) != 0x886c
		|| memcmp(data + 19, "\x00\x10\x18", 3) != 0
		|| get16be(data + 22) != 1) {
		return;
	}

	uint8* message = data + 24;
	uint16 flags = get16be(message + 2);
	uint32 type = get32be(message + 4);
	uint32 status = get32be(message + 8);
	uint32 reason = get32be(message + 12);
	uint32 dataLength = get32be(message + 20);
	uint8* address = message + 24;
	uint8* eventData = data + 72;
	if (dataLength > length - 72)
		dataLength = length - 72;

	switch (type) {
		case E_ESCAN_RESULT:
			_HandleScanResult(eventData, dataLength, status);
			return;

		case E_SET_SSID:
			if (status == E_STATUS_SUCCESS) {
				MutexLocker locker(fStateLock);
				memcpy(fBssid, address, 6);
				fAssociated = true;
				INFO("associated with %02x:%02x:%02x:%02x:%02x:%02x\n",
					address[0], address[1], address[2], address[3],
					address[4], address[5]);
				locker.Unlock();
				if (!fUsingSupplicant)
					_SetLink(true);
			} else {
				ERROR("joining failed (status %" B_PRIu32 ", reason %" B_PRIu32
					")\n", status, reason);
			}
			return;

		case E_LINK:
			if ((flags & 1) != 0) {
				MutexLocker locker(fStateLock);
				memcpy(fBssid, address, 6);
				fAssociated = true;
				locker.Unlock();
				if (!fUsingSupplicant || fKeyed)
					_SetLink(true);
			} else {
				{
					MutexLocker locker(fStateLock);
					fAssociated = false;
					fKeyed = false;
					memset(fBssid, 0, sizeof(fBssid));
				}
				_SetLink(false);
			}
			return;

		case E_PSK_SUP:
			if (status == E_STATUS_FWSUP_COMPLETED) {
				fKeyed = true;
				INFO("key exchange completed\n");
				if (fAssociated)
					_SetLink(true);
			} else if (status != 0) {
				ERROR("key exchange status %" B_PRIu32 " reason %" B_PRIu32
					"\n", status, reason);
			}
			return;

		case E_DEAUTH:
		case E_DEAUTH_IND:
		case E_DISASSOC:
		case E_DISASSOC_IND:
			INFO("disconnected (event %" B_PRIu32 ", reason %" B_PRIu32 ")\n",
				type, reason);
			{
				MutexLocker locker(fStateLock);
				fAssociated = false;
				fKeyed = false;
				memset(fBssid, 0, sizeof(fBssid));
			}
			_SetLink(false);
			return;

		default:
			if (status != E_STATUS_SUCCESS && status != E_STATUS_PARTIAL) {
				INFO("event %" B_PRIu32 ": status %" B_PRIu32 ", reason %"
					B_PRIu32 "\n", type, status, reason);
			} else
				TRACE("event %" B_PRIu32 "\n", type);
			return;
	}
}


void
Brcmf::_HandleScanResult(uint8* data, size_t length, uint32 status)
{
	if (status != E_STATUS_PARTIAL) {
		// scan finished (or was aborted)
		MutexLocker locker(fStateLock);
		if (!fScanning)
			return;
		fScanning = false;
		// forget about networks that were not seen in this scan
		for (int32 i = 0; i < fScanCount;) {
			if (!fScanResults[i]->seen) {
				free(fScanResults[i]);
				fScanResults[i] = fScanResults[--fScanCount];
			} else
				i++;
		}
		TRACE("scan done (status %" B_PRIu32 "), %" B_PRId32 " networks\n",
			status, fScanCount);
		locker.Unlock();
		_Notify(B_NETWORK_WLAN_SCANNED);
		return;
	}

	// buflen, version, sync id, bss count
	if (length < 12)
		return;
	uint16 count = get16(data + 10);
	uint8* bss = data + 12;
	size_t left = length - 12;

	MutexLocker locker(fStateLock);
	for (uint16 i = 0; i < count && left >= 128; i++) {
		uint32 bssLength = get32(bss + 4);
		if (bssLength < 128 || bssLength > left)
			break;

		const uint8* bssid = bss + 8;
		uint16 ieOffset = get16(bss + 116);
		uint32 ieLength = get32(bss + 120);
		if (ieOffset + ieLength > bssLength)
			ieLength = 0;

		brcmf_scan_entry* entry = NULL;
		for (int32 j = 0; j < fScanCount; j++) {
			if (memcmp(fScanResults[j]->bssid, bssid, 6) == 0) {
				entry = fScanResults[j];
				break;
			}
		}
		if (entry == NULL && fScanCount < BRCMF_MAX_SCAN_RESULTS) {
			entry = (brcmf_scan_entry*)malloc(sizeof(brcmf_scan_entry));
			if (entry != NULL)
				fScanResults[fScanCount++] = entry;
		}
		if (entry != NULL) {
			entry->seen = true;
			memcpy(entry->bssid, bssid, 6);
			entry->beaconInterval = get16(bss + 14);
			entry->capability = get16(bss + 16);
			entry->ssidLength = min_c(bss[18], 32);
			memcpy(entry->ssid, bss + 19, entry->ssidLength);
			uint32 rateCount = min_c(get32(bss + 52), (uint32)15);
			entry->rateCount = rateCount;
			memcpy(entry->rates, bss + 56, rateCount);
			uint8 channel = bss[88];
			if (channel == 0)
				channel = get16(bss + 72) & 0xff;
			entry->channel = channel;
			entry->rssi = (int16)get16(bss + 78);
			entry->noise = (int8)bss[80];
			entry->ieLength = min_c(ieLength, (uint32)sizeof(entry->ies));
			memcpy(entry->ies, bss + ieOffset, entry->ieLength);
		}

		bss += bssLength;
		left -= bssLength;
	}
}


// #pragma mark - management


status_t
Brcmf::StartScan(const uint8* ssid, size_t ssidLength)
{
	if (!fRunning)
		return B_DEV_NOT_READY;

	{
		MutexLocker locker(fStateLock);
		if (fScanning)
			return B_BUSY;
		fScanning = true;
		for (int32 i = 0; i < fScanCount; i++)
			fScanResults[i]->seen = false;
	}

	uint8 params[72];
	memset(params, 0, sizeof(params));
	put32(params, 1);				// version
	put16(params + 4, 1);			// action: start
	put16(params + 6, 0x1234);		// sync id
	if (ssid != NULL && ssidLength > 0 && ssidLength <= 32) {
		put32(params + 8, ssidLength);
		memcpy(params + 12, ssid, ssidLength);
	}
	memset(params + 44, 0xff, 6);	// bssid: any
	params[50] = 2;					// bss type: any
	params[51] = 0;					// active scan
	put32(params + 52, 0xffffffff);	// probes
	put32(params + 56, 0xffffffff);	// active time
	put32(params + 60, 0xffffffff);	// passive time
	put32(params + 64, 0xffffffff);	// home time
	put32(params + 68, 0);			// all channels, no SSID list

	_CmdInt(C_SET_PASSIVE_SCAN, 0);
	status_t status = _SetVar("escan", params, sizeof(params));
	if (status != B_OK) {
		ERROR("cannot start scan\n");
		MutexLocker locker(fStateLock);
		fScanning = false;
	}
	return status;
}


status_t
Brcmf::Join(const uint8* ssid, size_t ssidLength, uint32 authMode,
	const uint8* key, size_t keyLength)
{
	if (!fRunning)
		return B_DEV_NOT_READY;
	if (ssidLength == 0 || ssidLength > 32)
		return B_BAD_VALUE;

	if (fAssociated)
		Leave();

	uint32 wpaAuth = WPA_AUTH_DISABLED;
	uint32 wsec = WSEC_NONE;
	bool useSupplicant = false;
	switch (authMode) {
		case 0:		// none
			break;
		case 2:		// WPA
			wpaAuth = WPA_AUTH_WPA_PSK;
			wsec = WSEC_TKIP | WSEC_AES;
			useSupplicant = true;
			break;
		case 3:		// WPA2
			wpaAuth = WPA_AUTH_WPA2_PSK;
			wsec = WSEC_AES | WSEC_TKIP;
			useSupplicant = true;
			break;
		default:
			ERROR("authentication mode %" B_PRIu32 " not supported\n",
				authMode);
			return B_NOT_SUPPORTED;
	}
	if (useSupplicant && keyLength == 0)
		return B_BAD_VALUE;

	{
		MutexLocker locker(fStateLock);
		memcpy(fSsid, ssid, ssidLength);
		fSsidLength = ssidLength;
		fUsingSupplicant = useSupplicant;
		fKeyed = false;
	}

	_SetVarInt("wpa_auth", wpaAuth);
	_SetVarInt("wsec", wsec);
	_SetVarInt("auth", 0);
	_SetVarInt("mfp", 0);

	if (useSupplicant) {
		status_t status = _SetVarInt("sup_wpa", 1);
		if (status != B_OK) {
			ERROR("firmware does not support in-firmware WPA supplicant\n");
			return B_NOT_SUPPORTED;
		}

		// struct wsec_pmk: key length, flags, key[2 * 32 + 1]
		uint8 pmk[2 + 2 + 65];
		memset(pmk, 0, sizeof(pmk));
		if (keyLength == 32) {
			// a binary PMK: the firmware wants it as 64 hex digits
			static const char kHex[] = "0123456789abcdef";
			for (int i = 0; i < 32; i++) {
				pmk[4 + i * 2] = kHex[key[i] >> 4];
				pmk[4 + i * 2 + 1] = kHex[key[i] & 0xf];
			}
			put16(pmk, 64);
		} else if (keyLength >= 8 && keyLength <= 64) {
			memcpy(pmk + 4, key, keyLength);
			put16(pmk, keyLength);
		} else
			return B_BAD_VALUE;
		put16(pmk + 2, WSEC_PASSPHRASE);

		status = _Dcmd(C_SET_WSEC_PMK, true, pmk, sizeof(pmk), NULL);
		memset(pmk, 0, sizeof(pmk));
		if (status != B_OK) {
			ERROR("cannot set the key\n");
			return status;
		}
	} else
		_SetVarInt("sup_wpa", 0);

	INFO("joining \"%.*s\"%s\n", (int)ssidLength, ssid,
		useSupplicant ? " (firmware WPA supplicant)" : "");
	return _IssueJoin(ssid, ssidLength, NULL);
}


status_t
Brcmf::_IssueJoin(const uint8* ssid, size_t ssidLength, const uint8* bssid)
{
	static const uint8 kZero[6] = {};
	bool anyBssid = bssid == NULL || memcmp(bssid, kZero, 6) == 0;

	// extended join parameters: SSID, scan parameters, association
	uint8 join[68];
	memset(join, 0, sizeof(join));
	put32(join, ssidLength);
	memcpy(join + 4, ssid, ssidLength);
	join[36] = 0xff;				// scan type: default
	put32(join + 40, 0xffffffff);	// probes
	put32(join + 44, 0xffffffff);	// active time
	put32(join + 48, 0xffffffff);	// passive time
	put32(join + 52, 0xffffffff);	// home time
	if (anyBssid)
		memset(join + 56, 0xff, 6);
	else
		memcpy(join + 56, bssid, 6);
	put32(join + 64, 0);			// no channel list

	status_t status = _SetVar("join", join, sizeof(join));
	if (status != B_OK) {
		// older firmware: plain SET_SSID
		uint8 simple[48];
		memset(simple, 0, sizeof(simple));
		put32(simple, ssidLength);
		memcpy(simple + 4, ssid, ssidLength);
		if (anyBssid)
			memset(simple + 36, 0xff, 6);
		else
			memcpy(simple + 36, bssid, 6);
		status = _Dcmd(C_SET_SSID, true, simple, sizeof(simple), NULL);
	}
	return status;
}


/*!	Parses a WPA or RSN information element into the firmware's "wpa_auth"
	and "wsec" settings.
*/
static void
parse_wpa_ie(const uint8* ie, size_t length, uint32& wpaAuth, uint32& wsec)
{
	wpaAuth = WPA_AUTH_DISABLED;
	wsec = WSEC_NONE;
	if (length < 2 || ie[1] + 2u > length)
		return;

	bool rsn = ie[0] == 0x30;
	const uint8* p = ie + 2;
	const uint8* end = ie + 2 + ie[1];
	if (!rsn) {
		// vendor element: 00:50:f2 type 1
		if (ie[0] != 0xdd || end - p < 4 || memcmp(p, "\x00\x50\xf2\x01", 4))
			return;
		p += 4;
	}
	p += 2;		// version

	uint32 cipherBits = 0;
	auto cipher = [&](const uint8* suite) {
		switch (suite[3]) {
			case 1:
			case 5:
				cipherBits |= WSEC_WEP;
				break;
			case 2:
				cipherBits |= WSEC_TKIP;
				break;
			case 4:
				cipherBits |= WSEC_AES;
				break;
		}
	};

	// group cipher
	if (end - p < 4) {
		wsec = rsn ? WSEC_AES : WSEC_TKIP;
		wpaAuth = rsn ? WPA_AUTH_WPA2_PSK : WPA_AUTH_WPA_PSK;
		return;
	}
	cipher(p);
	p += 4;

	// pairwise ciphers
	if (end - p >= 2) {
		uint16 count = get16(p);
		p += 2;
		for (uint16 i = 0; i < count && end - p >= 4; i++, p += 4)
			cipher(p);
	}

	// key management
	bool psk = true;
	if (end - p >= 6) {
		uint16 count = get16(p);
		p += 2;
		psk = false;
		for (uint16 i = 0; i < count && end - p >= 4; i++, p += 4) {
			if (p[3] == 2 || p[3] == 6)
				psk = true;
		}
		if (!psk) {
			wpaAuth = rsn ? WPA_AUTH_WPA2_UNSPECIFIED
				: WPA_AUTH_WPA_UNSPECIFIED;
		}
	}
	if (psk)
		wpaAuth = rsn ? WPA_AUTH_WPA2_PSK : WPA_AUTH_WPA_PSK;
	wsec = cipherBits & (WSEC_TKIP | WSEC_AES);
}


status_t
Brcmf::JoinWithIE(const uint8* ssid, size_t ssidLength, const uint8* bssid,
	const uint8* ie, size_t ieLength, bool privacy)
{
	if (!fRunning)
		return B_DEV_NOT_READY;
	if (ssidLength == 0 || ssidLength > 32)
		return B_BAD_VALUE;

	uint32 wpaAuth, wsec;
	parse_wpa_ie(ie, ieLength, wpaAuth, wsec);
	if (wpaAuth == WPA_AUTH_DISABLED && privacy)
		wsec = WSEC_WEP;

	{
		MutexLocker locker(fStateLock);
		memcpy(fSsid, ssid, ssidLength);
		fSsidLength = ssidLength;
		fUsingSupplicant = false;
		fKeyed = false;
	}

	_SetVarInt("sup_wpa", 0);
	if (ieLength > 0)
		_SetVar("wpaie", ie, ieLength);
	_SetVarInt("wpa_auth", wpaAuth);
	_SetVarInt("wsec", wsec);
	_SetVarInt("auth", 0);
	_SetVarInt("mfp", 0);

	INFO("joining \"%.*s\" (wpa_auth %#" B_PRIx32 ", wsec %#" B_PRIx32
		", host supplicant)\n", (int)ssidLength, ssid, wpaAuth, wsec);
	return _IssueJoin(ssid, ssidLength, bssid);
}


/*!	Installs a key negotiated by the host supplicant. \a cipher uses the
	net80211 numbering (0 WEP, 1 TKIP, 3 AES-CCM).
*/
status_t
Brcmf::SetKey(const uint8* address, bool group, uint32 index, uint32 cipher,
	const uint8* key, size_t keyLength, uint64 rsc)
{
	if (!fRunning)
		return B_DEV_NOT_READY;
	if (keyLength > 32)
		return B_BAD_VALUE;

	uint32 algorithm;
	uint32 wsecBit;
	switch (cipher) {
		case 0:
			algorithm = keyLength == 5 ? 1 : 3;		// WEP1 / WEP128
			wsecBit = WSEC_WEP;
			break;
		case 1:
			algorithm = 2;							// TKIP
			wsecBit = WSEC_TKIP;
			break;
		case 3:
			algorithm = 4;							// AES-CCM
			wsecBit = WSEC_AES;
			break;
		default:
			return B_NOT_SUPPORTED;
	}

	// struct wsec_key
	uint8 data[164];
	memset(data, 0, sizeof(data));
	put32(data, index);
	put32(data + 4, keyLength);
	memcpy(data + 8, key, keyLength);
	if (cipher == 1 && keyLength == 32) {
		// the firmware wants the Michael keys the other way around
		uint8 mic[8];
		memcpy(mic, data + 8 + 24, 8);
		memcpy(data + 8 + 24, data + 8 + 16, 8);
		memcpy(data + 8 + 16, mic, 8);
	}
	put32(data + 112, algorithm);
	put32(data + 116, group ? 2 : 0);		// primary key
	put32(data + 132, 1);					// IV initialized
	put32(data + 140, rsc >> 16);
	put16(data + 144, rsc & 0xffff);
	if (!group && address != NULL)
		memcpy(data + 156, address, 6);

	uint32 wsec = 0;
	if (_GetVarInt("wsec", &wsec) == B_OK && (wsec & wsecBit) == 0)
		_SetVarInt("wsec", wsec | wsecBit);

	status_t status = _SetVar("wsec_key", data, sizeof(data));
	memset(data, 0, sizeof(data));
	if (status != B_OK)
		ERROR("setting %s key failed\n", group ? "group" : "pairwise");
	else
		INFO("%s key installed\n", group ? "group" : "pairwise");
	return status;
}


status_t
Brcmf::Leave()
{
	if (!fRunning)
		return B_DEV_NOT_READY;

	status_t status = _Dcmd(C_DISASSOC, true, NULL, 0, NULL);
	{
		MutexLocker locker(fStateLock);
		fAssociated = false;
		fKeyed = false;
		fSsidLength = 0;
		memset(fBssid, 0, sizeof(fBssid));
	}
	_SetLink(false);
	return status;
}
