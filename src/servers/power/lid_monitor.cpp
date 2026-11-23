/*
 * Copyright 2013, Jérôme Duval, korli@users.berlios.de.
 * Copyright 2005, Nathan Whitehorn.
 * Copyright 2026, Andrea Bernardi
 *
 * Distributed under the terms of the MIT License.
 */


#include "lid_monitor.h"

#include <Accelerant.h>
#include <FindDirectory.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Path.h>
#include <Screen.h>
#include <Server.h>

#include <dirent.h>
#include <driver_settings.h>
#include <fcntl.h>
#include <net/if.h>
#include <scheduler.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>


/*!	Switches the screen on or off. The daemon starts without a connection to
	the app_server, which may not run yet then: it connects the first time.
*/
static void
set_screen_power(bool on)
{
	BServer* server = dynamic_cast<BServer*>(be_app);
	if (server == NULL || server->InitGUIContext() != B_OK)
		return;

	BScreen screen(B_MAIN_SCREEN_ID);
	if (screen.IsValid())
		screen.SetDPMS(on ? B_DPMS_ON : B_DPMS_OFF);
}


// Each lid driver publishes power/<name>_lid/0 (acpi_lid for ACPI): one
// byte, 0 while the lid is closed, readable when it changed
static const char* const kPowerDevices = "/dev/power";


LidMonitor::LidMonitor()
	:
	fClosed(false),
	fLowPower(true),
	fSchedulerMode(-1)
{
	DIR* dir = opendir(kPowerDevices);
	if (dir != NULL) {
		dirent* entry;
		while ((entry = readdir(dir)) != NULL) {
			size_t length = strlen(entry->d_name);
			if (length < 4 || strcmp(entry->d_name + length - 4, "_lid") != 0)
				continue;
			BString path;
			path.SetToFormat("%s/%s/0", kPowerDevices, entry->d_name);
			int fd = open(path.String(), O_RDONLY);
			if (fd >= 0)
				fFDs.insert(fd);
		}
		closedir(dir);
	}

	// "lid_closed_low_power false" in ~/config/settings/power_daemon keeps
	// everything running while the lid is closed
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("power_daemon") == B_OK) {
		void* settings = load_driver_settings(path.Path());
		if (settings != NULL) {
			fLowPower = get_driver_boolean_parameter(settings,
				"lid_closed_low_power", true, true);
			unload_driver_settings(settings);
		}
	}
}


LidMonitor::~LidMonitor()
{
	if (fClosed)
		_LidOpened();

	for (std::set<int>::iterator it = fFDs.begin(); it != fFDs.end(); ++it)
		close(*it);
}


void
LidMonitor::HandleEvent(int fd)
{
	uint8 status;
	if (read(fd, &status, 1) != 1)
		return;

	// 0 when the lid is closed, as ACPI's _LID
	bool closed = status == 0;
	if (closed == fClosed)
		return;
	fClosed = closed;

	syslog(LOG_INFO, "power_daemon: lid %s\n", closed ? "closed" : "open");
	if (!fLowPower)
		return;

	if (closed)
		_LidClosed();
	else
		_LidOpened();
}


/*!	While the lid is closed, nobody looks at the screen or types: the screen
	is switched off (DPMS, which also cuts the panel where the driver can),
	the scheduler saves power, and the wireless networks are left.
*/
void
LidMonitor::_LidClosed()
{
	set_screen_power(false);

	fSchedulerMode = get_scheduler_mode();
	set_scheduler_mode(SCHEDULER_MODE_POWER_SAVING);

	fInterfacesDown.MakeEmpty();
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		int32 flags = interface.Flags();
		if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0)
			continue;
		BNetworkDevice device(interface.Name());
		if (!device.IsWireless())
			continue;
		if (interface.SetFlags(flags
				& ~(IFF_UP | IFF_AUTO_CONFIGURED | IFF_CONFIGURING)) == B_OK)
			fInterfacesDown.Add(interface.Name());
	}

	syslog(LOG_INFO, "power_daemon: lid closed: screen off, power saving, "
		"%" B_PRId32 " wireless interfaces down\n",
		fInterfacesDown.CountStrings());
}


void
LidMonitor::_LidOpened()
{
	if (fSchedulerMode >= 0) {
		set_scheduler_mode(fSchedulerMode);
		fSchedulerMode = -1;
	}

	// the network server joins the known networks again, and asks for an
	// address, as at boot
	for (int32 i = 0; i < fInterfacesDown.CountStrings(); i++) {
		BNetworkInterface interface(fInterfacesDown.StringAt(i).String());
		interface.SetFlags(interface.Flags() | IFF_UP);
		interface.AutoConfigure(AF_INET);
	}
	fInterfacesDown.MakeEmpty();

	// last, as it waits for the app_server
	set_screen_power(true);
}
