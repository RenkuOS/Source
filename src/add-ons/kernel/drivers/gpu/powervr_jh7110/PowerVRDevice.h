/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_DEVICE_H
#define POWERVR_DEVICE_H


#include <condition_variable.h>
#include <lock.h>

#include "pvr_context.h"


#define POWERVR_TRACE(x...)	dprintf("powervr_jh7110: " x)


/*!	Pages of one physically contiguous area below 4 GB, for everything the
	firmware or the GPU reads before any client mapping exists: the firmware
	image, the MIPS page table, the firmware interface objects, the client
	CCBs and the page tables of the GPU MMU. The MIPS firmware reaches its
	objects through its own page table over a 16 MB heap; FirmwareHeap keeps
	the physical pages and the heap offsets apart.
*/
class FirmwareHeap {
public:
								FirmwareHeap();
								~FirmwareHeap();

			status_t			Init(size_t size);
			void				Uninit();

			status_t			AllocatePages(size_t size, size_t alignment,
									uint8** _cpu, phys_addr_t* _phys);
			void				FreePages(void* cpu, size_t size);

			status_t			AllocateOffset(size_t size, size_t alignment,
									uint32* _offset);
			status_t			ReserveOffset(uint32 offset, size_t size);
			void				FreeOffset(uint32 offset, size_t size);

			uint8*				Base() const { return fBase; }
			phys_addr_t			PhysicalBase() const { return fPhysicalBase; }
			size_t				Size() const { return fSize; }

private:
			status_t			_Find(uint8* bitmap, uint32 count,
									uint32 pages, uint32 alignPages,
									uint32 first, uint32* _index);
			void				_Mark(uint8* bitmap, uint32 index,
									uint32 pages, bool used);

			area_id				fArea;
			uint8*				fBase;
			phys_addr_t			fPhysicalBase;
			size_t				fSize;
			uint8*				fPageBitmap;
			uint32				fPageCount;
			uint8				fOffsetBitmap[4096 / 8];
				// 4 KB pages of the 16 MB firmware heap
};


// called by ProcessFirmwareEvents() for a context reset notification
typedef void (*powervr_reset_hook)(void* cookie, uint32 contextID,
	uint32 reason, uint32 flags, uint64 faultAddress);
// a firmware CCB command the device does not handle itself (free list
// grow and reconstruction requests); called with the device lock held
typedef void (*powervr_event_hook)(void* cookie, uint32 type,
	const uint32* words);


/*!	The GPU, its firmware and the kernel side of the firmware interface. One
	instance; started when the first client opens the device, stopped when
	the last one goes. Every method but the interrupt handler, Sync() and
	the register accessors runs with Lock() held.
*/
class PowerVRDevice {
public:
								PowerVRDevice();
								~PowerVRDevice();

			status_t			Start();
			// halts the firmware and the GPU; the firmware memory stays
			// until ReleaseMemory(), so objects the firmware held can still
			// be freed into it
			void				Stop();
			void				ReleaseMemory();
			bool				IsRunning() const { return fRunning; }
			bool				IsLost() const { return fLost; }

			void				SetResetHook(powervr_reset_hook hook,
									void* cookie)
									{ fResetHook = hook;
										fResetCookie = cookie; }
			void				SetEventHook(powervr_event_hook hook,
									void* cookie)
									{ fEventHook = hook;
										fEventCookie = cookie; }

			void				Lock() { mutex_lock(&fLock); }
			void				Unlock() { mutex_unlock(&fLock); }
			mutex*				LockObject() { return &fLock; }

			const pvr_ctx_env&	ContextEnvironment() const
									{ return fContextEnvironment; }
			const pvr_mmu_env&	MMUEnvironment() const
									{ return fMMUEnvironment; }
			pvr_fwif_state*		Firmware() { return &fFirmware; }

			uint64				BVNC() const { return fBVNC; }
			uint16				FirmwareMajor() const { return fFirmwareMajor; }
			uint16				FirmwareMinor() const { return fFirmwareMinor; }
			uint32				FirmwareBuild() const { return fFirmwareBuild; }
			uint32				CoreClock() const { return fCoreClock; }

			// write back and invalidate the L2 cache lines of a range
			void				Sync(phys_addr_t physical, size_t size);

			// waits until the next firmware interrupt or the timeout
			status_t			WaitForEvent(bigtime_t timeout);
			void				ProcessFirmwareEvents();

			uint32				NextContextID() { return ++fLastContextID; }

			uint32				interrupts;
			uint32				mmuPageFaults;
			uint32				hardwareRecoveries;
			uint32				firmwareStarts;

			// hooks of the rogue modules
	static	status_t			FirmwareAllocate(void* cookie, size_t size,
									size_t alignment, uint32 heapOffset,
									void** _cpu, uint64* _phys,
									uint32* _fwAddress);
	static	void				FirmwareFree(void* cookie, void* cpu,
									uint64 phys, uint32 fwAddress,
									size_t size);
	static	status_t			GPUAllocate(void* cookie, size_t size,
									size_t alignment, void** _cpu,
									uint64* _phys);
	static	void				GPUFree(void* cookie, void* cpu, uint64 phys,
									size_t size);
	static	void				CacheSync(void* cookie, uint64 phys,
									size_t size);
	static	void				MTSSchedule(void* cookie, uint32 value);
	static	uint64				ReadRegister64Hook(void* cookie,
									uint32 offset);
	static	void				Log(void* cookie, const char* format, ...);

private:
			uint32				_Read32(uint32 offset) const
									{ return *(volatile uint32*)
										(fRegisters + offset); }
			uint64				_Read64(uint32 offset) const
									{ return *(volatile uint64*)
										(fRegisters + offset); }
			void				_Write32(uint32 offset, uint32 value)
									{ *(volatile uint32*)(fRegisters + offset)
										= value; }
			void				_Write64(uint32 offset, uint64 value)
									{ *(volatile uint64*)(fRegisters + offset)
										= value; }

			status_t			_MapRegisters();
			void				_UnmapRegisters();
			status_t			_PowerUp();
			void				_PowerDown();
			status_t			_LoadFirmware();
			status_t			_PrepareFirmware();
			void				_MapFirmwarePages(uint32 heapOffset,
									phys_addr_t phys, size_t size);
			status_t			_StartFirmware();
			status_t			_StopFirmware();
			status_t			_HealthCheck();

	static	int32				_HandleInterrupt(void* data);
	static	status_t			_EventWorker(void* data);

			status_t			_MeasureCoreClock();

			mutex				fLock;
			bool				fRunning;
			bool				fLost;
			powervr_reset_hook	fResetHook;
			void*				fResetCookie;
			powervr_event_hook	fEventHook;
			void*				fEventCookie;
			thread_id			fEventWorker;
			bool				fEventWorkerQuit;

			area_id				fRegisterAreas[4];
			volatile uint8*		fRegisters;
			volatile uint32*	fPMU;
			volatile uint32*	fClocks;
			volatile uint64*	fFlush64;

			uint8*				fImage;
			size_t				fImageSize;

			FirmwareHeap		fHeap;
			uint32*				fPageTable;
			phys_addr_t			fPageTablePhysical;
			phys_addr_t			fCodePhysical;
			phys_addr_t			fDataPhysical;
			phys_addr_t			fBootCodePhysical;
			phys_addr_t			fBootDataPhysical;
			phys_addr_t			fExceptionPhysical;

			pvr_fwif_state		fFirmware;
			pvr_ctx_env			fContextEnvironment;
			pvr_mmu_env			fMMUEnvironment;
			bool				fInterruptInstalled;
			ConditionVariable	fEvent;

			uint64				fBVNC;
			uint16				fFirmwareMajor;
			uint16				fFirmwareMinor;
			uint32				fFirmwareBuild;
			uint32				fCoreClock;
			uint32				fLastContextID;
};


#endif	// POWERVR_DEVICE_H
