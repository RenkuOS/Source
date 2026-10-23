/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 *
 * Native driver for the Broadcom/Cypress FullMAC SDIO WLAN chips found on the
 * Raspberry Pi 4 (CYW43455) and Pi 400 (CYW43456), attached to the "Arasan"
 * SDHCI controller of the BCM2711 (ACPI HID "BCM2847").
 *
 * The chip handling and the SDPCM/BCDC protocol follow OpenBSD's bwfm(4)
 * (sys/dev/ic/bwfm.c, sys/dev/sdmmc/if_bwfm_sdio.c) and Plan 9's ether4330.
 */
#ifndef BRCMFMAC_H
#define BRCMFMAC_H


#include <KernelExport.h>
#include <OS.h>

#include <condition_variable.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>


//#define TRACE_BRCMFMAC
#ifdef TRACE_BRCMFMAC
#	define TRACE(x...) dprintf("brcmfmac: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...) dprintf("brcmfmac: " x)
#define ERROR(x...) dprintf("brcmfmac: " x)


extern net_buffer_module_info* gBufferModule;


// #pragma mark - VideoCore firmware helpers (vc_mailbox.cpp)


bool		is_raspberry_pi_firmware();
status_t	vc_mailbox_property(uint32 tag, uint32* values, uint32 valueWords);
uint32		vc_get_clock_rate(uint32 clockId);
status_t	vc_set_gpio(uint32 gpio, uint32 state);
status_t	vc_get_gpio(uint32 gpio, uint32* state);
void		bcm2711_wifi_pin_setup();


// #pragma mark - SDIO host (sdio_host.cpp)


enum {
	SDIO_RESP_NONE		= 0,
	SDIO_RESP_R1		= 1,	// also R5, R6
	SDIO_RESP_R1B		= 2,
	SDIO_RESP_R4		= 3,	// no CRC, no index check
};


class SdioHost {
public:
								SdioHost(addr_t registers, uint32 baseClock);

			status_t			Reset();
			void				SetClock(uint32 hertz);
			void				SetBusWidth(int width);

			status_t			Command(uint8 index, uint32 argument,
									uint32 responseType, uint32* response);

			// SDIO commands
			status_t			ReadDirect(int function, uint32 address,
									uint8* _value);
			status_t			WriteDirect(int function, uint32 address,
									uint8 value);
			status_t			Extended(bool write, int function,
									uint32 address, bool increment,
									void* buffer, size_t length);
			void				SetBlockSize(int function, uint32 size)
									{ fBlockSize[function] = size; }

			// card interrupt
			void				EnableCardInterrupt(bool enable);
			bool				InterruptPending();

			uint32				Version() const { return fVersion; }
			uint32				BaseClock() const { return fBaseClock; }

private:
			uint32				_Read(uint32 offset)
									{ return fRegisters[offset / 4]; }
			void				_Write(uint32 offset, uint32 value);
			void				_WriteData(uint32 value)
									{ fRegisters[0x20 / 4] = value; }

			void				_ResetLines(uint32 bits);
			status_t			_WaitStatusClear(uint32 bits,
									bigtime_t timeout);
			status_t			_WaitInterrupt(uint32 bits, bigtime_t timeout,
									uint32* _status);
			status_t			_SendCommand(uint8 index, uint32 argument,
									uint32 responseType, bool data, bool read,
									uint32 blockSize, uint32 blockCount,
									uint32* response);
			status_t			_Transfer(bool read, uint8* buffer,
									uint32 blockSize, uint32 blockCount);
			status_t			_ExtendedChunk(bool write, int function,
									uint32 address, bool increment,
									bool blockMode, uint8* buffer,
									uint32 count, uint32 blockSize);

private:
	volatile	uint32*			fRegisters;
			uint32				fBaseClock;
			uint32				fClock;
			uint32				fVersion;
			uint32				fBlockSize[8];
			spinlock			fInterruptLock;
};


// #pragma mark - chip and protocol (brcmf.cpp)


struct brcmf_core {
	uint16		id;
	uint8		rev;
	uint32		base;
	uint32		wrap;
};


struct brcmf_scan_entry {
	bool		seen;
	uint8		bssid[6];
	uint8		ssid[32];
	uint8		ssidLength;
	uint16		capability;
	uint16		beaconInterval;
	uint16		channel;
	int16		rssi;
	int8		noise;
	uint8		rateCount;
	uint8		rates[15];
	uint16		ieLength;
	uint8		ies[1024];
};


#define BRCMF_MAX_SCAN_RESULTS	64
#define BRCMF_MAX_CORES			32
#define BRCMF_RX_QUEUE_MAX		256
#define BRCMF_MAX_FRAME			2048
#define BRCMF_BUFFER_SIZE		(64 * 1024)


class Brcmf {
public:
								Brcmf(SdioHost* host);
								~Brcmf();

			// bring up
			status_t			Probe();
			status_t			Start();
			bool				IsRunning() const { return fRunning; }
			void				Stop();

			// data path
			status_t			Send(net_buffer* buffer);
			status_t			Receive(net_buffer** _buffer, bool block);

			// management
			status_t			StartScan(const uint8* ssid, size_t ssidLength);
			bool				IsScanning() const { return fScanning; }
			status_t			Join(const uint8* ssid, size_t ssidLength,
									uint32 authMode, const uint8* key,
									size_t keyLength);
			status_t			Leave();
			status_t			JoinWithIE(const uint8* ssid,
									size_t ssidLength, const uint8* bssid,
									const uint8* ie, size_t ieLength,
									bool privacy);
			status_t			SetKey(const uint8* address, bool group,
									uint32 index, uint32 cipher,
									const uint8* key, size_t keyLength,
									uint64 rsc);

			// state (take fStateLock via Lock()/Unlock() when reading)
			void				LockState() { mutex_lock(&fStateLock); }
			void				UnlockState() { mutex_unlock(&fStateLock); }
			int32				CountScanResults() const
									{ return fScanCount; }
			const brcmf_scan_entry* ScanResultAt(int32 index) const
									{ return fScanResults[index]; }

			const uint8*		MacAddress() const { return fMacAddress; }
			bool				IsLinkUp() const { return fLinkUp; }
			const uint8*		Bssid() const { return fBssid; }
			const uint8*		Ssid() const { return fSsid; }
			uint8				SsidLength() const { return fSsidLength; }
			int32				Rssi() const { return fRssi; }
			bool				HasFirmwareSupplicant() const
									{ return fHasFirmwareSupplicant; }

			void				SetLinkStateSemaphore(sem_id sem)
									{ fLinkStateSem = sem; }
			void				SetDeviceName(const char* name);
			void				SetPromiscuous(bool enable);
			void				InterruptReceived();

private:
			// backplane access
			uint8				_Read1(uint32 address);
			void				_Write1(uint32 address, uint8 value);
			uint32				_Read4(uint32 address);
			void				_Write4(uint32 address, uint32 value);
			void				_SetWindow(uint32 address);
			status_t			_RamReadWrite(uint32 address, uint8* data,
									size_t length, bool write);

			// chip
			status_t			_SdioInit();
			status_t			_ChipAttach();
			void				_EromScan();
			bool				_EromGetRegAddress(uint32& erom,
									uint32& base, uint32& wrap);
			brcmf_core*			_Core(uint16 id, int index = 0);
			brcmf_core*			_Pmu();
			bool				_CoreIsUp(brcmf_core* core);
			void				_CoreDisable(brcmf_core* core,
									uint32 prereset, uint32 reset);
			void				_CoreReset(brcmf_core* core,
									uint32 prereset, uint32 reset,
									uint32 postreset);
			void				_SetPassive();
			status_t			_SetActive(uint32 resetVector);
			status_t			_WaitClock(bool alpOnly);
			status_t			_LoadFirmware();
			status_t			_ReadFile(const char* name, uint8** _data,
									size_t* _size);
			status_t			_ConvertNvram(uint8* data, size_t size,
									uint8** _nvram, size_t* _length);
			uint32				_DevRead(uint32 reg);
			void				_DevWrite(uint32 reg, uint32 value);

			// protocol
			static status_t		_ReaderThread(void* self);
			void				_ReaderLoop();
			void				_ServiceInterrupts();
			void				_ReadFrames();
			status_t			_FrameReadWrite(uint8* buffer, size_t length,
									bool write);
			status_t			_SendFrame(uint8 channel, const uint8* header,
									size_t headerLength, net_buffer* buffer,
									const uint8* data, size_t length);
			bool				_TxWindowOpen() const;
			void				_HandleControl(uint8* data, size_t length);
			void				_HandleEvent(uint8* data, size_t length);
			void				_HandleData(uint8* data, size_t length);
			void				_HandleScanResult(uint8* data, size_t length,
									uint32 status);
			void				_SetLink(bool up);
			void				_Notify(int32 opcode);

			status_t			_Dcmd(uint32 command, bool set, void* data,
									size_t length, size_t* _replyLength);
			status_t			_GetVar(const char* name, void* data,
									size_t length);
			status_t			_SetVar(const char* name, const void* data,
									size_t length);
			status_t			_GetVarInt(const char* name, uint32* value);
			status_t			_SetVarInt(const char* name, uint32 value);
			status_t			_CmdInt(uint32 command, uint32 value);
			status_t			_IssueJoin(const uint8* ssid,
									size_t ssidLength, const uint8* bssid);
			status_t			_LoadClm();
			status_t			_InitFirmware();

private:
			SdioHost*			fHost;
			mutex				fBusLock;
			mutex				fStateLock;
			mutex				fCommandLock;
			uint32				fWindow;

			uint16				fChip;
			uint8				fChipRev;
			brcmf_core			fCores[BRCMF_MAX_CORES];
			int32				fCoreCount;
			uint32				fRamBase;
			uint32				fRamSize;
			uint32				fCcCaps;
			uint32				fCcCapsExt;
			uint32				fSdioCoreBase;

			uint8*				fBounce;		// bus transfers
			uint8*				fTxBuffer;		// frame assembly
			bool				fRunning;
			bool				fProbed;

			// protocol state
			thread_id			fReaderThread;
			volatile bool		fStopReader;
			sem_id				fInterruptSem;
			uint8				fTxSeq;
			uint8				fTxMax;
			uint8				fFlowControl;
			ConditionVariable	fTxCondition;

			uint16				fRequestId;
			uint16				fPendingId;
			uint8*				fReply;
			size_t				fReplyLength;
			bool				fReplyDone;
			ConditionVariable	fReplyCondition;

			net_buffer*			fRxQueue[BRCMF_RX_QUEUE_MAX];
			int32				fRxHead;
			int32				fRxCount;
			ConditionVariable	fRxCondition;
			spinlock			fRxLock;

			// wifi state
			uint8				fMacAddress[6];
			bool				fLinkUp;
			bool				fAssociated;
			bool				fUsingSupplicant;
			bool				fKeyed;
			uint8				fBssid[6];
			uint8				fSsid[32];
			uint8				fSsidLength;
			int32				fRssi;
			uint32				fScanVersion;
			bool				fHasFirmwareSupplicant;
			bool				fScanning;
			brcmf_scan_entry*	fScanResults[BRCMF_MAX_SCAN_RESULTS];
			int32				fScanCount;
			sem_id				fLinkStateSem;
			char				fDeviceName[64];
			char				fFirmwareBase[32];
};


#endif	// BRCMFMAC_H
