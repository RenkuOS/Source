/*
 * Copyright 2012 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		François Revol, revol@free.fr
 */
#ifndef _KERNEL_ARCH_DEBUG_UART_H
#define _KERNEL_ARCH_DEBUG_UART_H


#include <sys/types.h>

#include <SupportDefs.h>


class DebugUART {
public:
#if defined(__ARM__) || defined(__aarch64__)
	static const uint8		kDefaultRegShift = 2;
#else
	static const uint8		kDefaultRegShift = 0;
#endif

							DebugUART(addr_t base, int64 clock)
								: fBase(base),
								fClock(clock),
								fRegShift(kDefaultRegShift),
								fRegIOWidth(1),
								fEnabled(true) {};
							~DebugUART() {};

	virtual	void			InitEarly() {};
	virtual	void			Init() {};
	virtual	void			InitPort(uint32 baud) {};

	virtual	void			Enable() { fEnabled = true; }
	virtual	void			Disable() { fEnabled = false; }

	virtual	int				PutChar(char c) = 0;
	virtual	int				GetChar(bool wait) = 0;

	virtual	void			FlushTx() = 0;
	virtual	void			FlushRx() = 0;

			void			SetBase(addr_t base) { fBase = base; }
			addr_t			Base() const { return fBase; }
			int64			Clock() const { return fClock; }
			void			SetRegisterLayout(uint8 regShift,
								uint8 regIOWidth);
			uint8			RegShift() const { return fRegShift; }
			uint8			RegIOWidth() const { return fRegIOWidth; }
			bool			Enabled() const { return fEnabled; }

protected:
	virtual	void			Out8(int reg, uint8 value);
	virtual	uint8			In8(int reg);
	virtual	void			Barrier();

private:
			addr_t			fBase;
			int64			fClock;
			uint8			fRegShift;
			uint8			fRegIOWidth;
			bool			fEnabled;
};


#endif	/* _KERNEL_ARCH_DEBUG_UART_H */
