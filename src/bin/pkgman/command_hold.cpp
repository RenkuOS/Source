/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include <StringList.h>

#include "Command.h"
#include "pkgman.h"
#include "PackageManager.h"


// TODO: internationalization!


using namespace BPackageKit;
using namespace BPackageKit::BPrivate;


static const char* const kHoldShortUsage =
	"  %command% [ <package> ... ]\n"
	"    Holds packages at the version installed, or lists the held ones.\n";

static const char* const kHoldLongUsage =
	"Usage: %program% %command% [ <package> ... ]\n"
	"Holds the specified installed packages at the version installed:\n"
	"\"update\", \"full-sync\" and \"reinstall\" leave them as they are, and\n"
	"no repository version of them is installed. A package file can still be\n"
	"installed over them on purpose. Without packages, lists the held\n"
	"packages.\n"
	"\n"
	"Options:\n"
	"  -H, --home\n"
	"    Hold packages in the user's home directory. Default is to hold\n"
	"    packages in the system directory.\n"
	"\n";

static const char* const kUnholdShortUsage =
	"  %command% <package> ...\n"
	"    Lets held packages be updated again.\n";

static const char* const kUnholdLongUsage =
	"Usage: %program% %command% <package> ...\n"
	"Releases the specified held packages, so that \"update\" and\n"
	"\"full-sync\" change them again.\n"
	"\n"
	"Options:\n"
	"  -H, --home\n"
	"    Release packages in the user's home directory. Default is to release\n"
	"    packages in the system directory.\n"
	"\n";


DEFINE_COMMAND(HoldCommand, "hold", kHoldShortUsage, kHoldLongUsage,
	COMMAND_CATEGORY_PACKAGES)
DEFINE_COMMAND(UnholdCommand, "unhold", kUnholdShortUsage, kUnholdLongUsage,
	COMMAND_CATEGORY_PACKAGES)


static int
hold_packages(Command& command, CommonOptions& commonOptions, int argc,
	const char* const* argv, bool held)
{
	BPackageInstallationLocation location
		= B_PACKAGE_INSTALLATION_LOCATION_SYSTEM;

	while (true) {
		static struct option sLongOptions[] = {
			{ "debug", required_argument, 0, OPTION_DEBUG },
			{ "help", no_argument, 0, 'h' },
			{ "home", no_argument, 0, 'H' },
			{ 0, 0, 0, 0 }
		};

		opterr = 0; // don't print errors
		int c = getopt_long(argc, (char**)argv, "hH", sLongOptions, NULL);
		if (c == -1)
			break;

		if (commonOptions.HandleOption(c))
			continue;

		switch (c) {
			case 'h':
				command.PrintUsageAndExit(false);
				break;

			case 'H':
				location = B_PACKAGE_INSTALLATION_LOCATION_HOME;
				break;

			default:
				command.PrintUsageAndExit(true);
				break;
		}
	}

	PackageManager packageManager(location, false);
	packageManager.SetDebugLevel(commonOptions.DebugLevel());

	// Without packages, "hold" lists the held ones.
	if (argc < optind + 1) {
		if (held) {
			BStringList packages;
			packageManager.GetHeldPackages(packages);
			for (int32 i = 0; i < packages.CountStrings(); i++)
				printf("%s\n", packages.StringAt(i).String());
			return 0;
		}
		command.PrintUsageAndExit(true);
	}

	packageManager.SetHeld(argv + optind, argc - optind, held);
	return 0;
}


int
HoldCommand::Execute(int argc, const char* const* argv)
{
	return hold_packages(*this, fCommonOptions, argc, argv, true);
}


int
UnholdCommand::Execute(int argc, const char* const* argv)
{
	return hold_packages(*this, fCommonOptions, argc, argv, false);
}
