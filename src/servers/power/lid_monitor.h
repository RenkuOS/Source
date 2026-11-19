/*
 * Copyright 2005-2026, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Nathan Whitehorn
 */
#ifndef _LID_MONITOR_H
#define _LID_MONITOR_H


#include <String.h>
#include <StringList.h>

#include "power_monitor.h"


class LidMonitor : public PowerMonitor {
public:
								LidMonitor();
	virtual	 					~LidMonitor();

	virtual	void				HandleEvent(int fd);

	virtual	const std::set<int>&
								FDs() const { return fFDs; }
private:
			void				_LidClosed();
			void				_LidOpened();

			std::set<int>		fFDs;
			bool				fClosed;
			bool				fLowPower;
				// what the settings ask for when the lid closes
			int32				fSchedulerMode;
				// before the lid closed
			BStringList			fInterfacesDown;
				// the wireless interfaces taken down when it closed
};


#endif // _LID_MONITOR_H
