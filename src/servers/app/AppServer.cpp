/*
 * Copyright 2001-2016, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 *		Axel Dörfler, axeld@pinc-software.de
 *		Stephan Aßmus <superstippi@gmx.de>
 * 		Christian Packmann
 */


#include "AppServer.h"

#include <syslog.h>

#include <AutoDeleter.h>
#include <LaunchRoster.h>
#include <PortLink.h>
#include <RosterPrivate.h>

#include "BitmapManager.h"
#include "Desktop.h"
#include "GlobalFontManager.h"
#include "InputManager.h"
#include "ScreenManager.h"
#include "ServerProtocol.h"


//#define DEBUG_SERVER
#ifdef DEBUG_SERVER
#	include <stdio.h>
#	define STRACE(x) printf x
#else
#	define STRACE(x) ;
#endif


// Globals
port_id gAppServerPort;
BTokenSpace gTokenSpace;
uint32 gAppServerSIMDFlags = 0;


/*!	\brief Constructor

	This loads the default fonts, allocates all the major global variables,
	spawns the main housekeeping threads, loads user preferences for the UI
	and decorator, and allocates various locks.
*/
AppServer::AppServer(status_t* status)
	:
	SERVER_BASE("application/x-vnd.Haiku-app_server", "picasso", -1, false,
		status),
	fDesktopLock("AppServerDesktopLock")
{
	open KEVIN WAS HERE. THIS IS A TEST. IT SHOULD NOT COMPILE, AND FAIL CI.

	gInputManager = new InputManager();

	// Create the
	srand(real_time_clock_usecs());

	status_t status;
	AppServer* server = new AppServer(&status);
	if (status == B_OK)
		server->Run();

	return status == B_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
