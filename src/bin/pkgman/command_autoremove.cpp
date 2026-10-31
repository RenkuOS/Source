/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "Command.h"
#include "pkgman.h"
#include "PackageManager.h"


// TODO: internationalization!


using namespace BPackageKit;
using namespace BPackageKit::BPrivate;


static const char* const kShortUsage =
	"  %command%\n"
	"    Uninstalls dependencies that are no longer needed.\n";

static const char* const kLongUsage =
	"Usage: %program% %command%\n"
	"Uninstalls the packages that were installed only as dependencies of\n"
	"other packages and that no installed package needs anymore.\n"
	"\n"
	"Options:\n"
	"  --debug <level>\n"
	"    Print debug output. <level> should be between 0 (no debug output,\n"
	"    the default) and 10 (most debug output).\n"
	"  -H, --home\n"
	"    Uninstall the packages from the user's home directory. Default is to\n"
	"    uninstall from the system directory.\n"
	"  -y\n"
	"    Non-interactive mode. Automatically confirm changes, but fail when\n"
	"    encountering problems.\n"
	"\n";


DEFINE_COMMAND(AutoremoveCommand, "autoremove", kShortUsage, kLongUsage,
	COMMAND_CATEGORY_PACKAGES)


int
AutoremoveCommand::Execute(int argc, const char* const* argv)
{
	BPackageInstallationLocation location
		= B_PACKAGE_INSTALLATION_LOCATION_SYSTEM;
	bool interactive = true;

	while (true) {
		static struct option sLongOptions[] = {
			{ "debug", required_argument, 0, OPTION_DEBUG },
			{ "help", no_argument, 0, 'h' },
			{ "home", no_argument, 0, 'H' },
			{ 0, 0, 0, 0 }
		};

		opterr = 0; // don't print errors
		int c = getopt_long(argc, (char**)argv, "hHy", sLongOptions, NULL);
		if (c == -1)
			break;

		if (fCommonOptions.HandleOption(c))
			continue;

		switch (c) {
			case 'h':
				PrintUsageAndExit(false);
				break;

			case 'H':
				location = B_PACKAGE_INSTALLATION_LOCATION_HOME;
				break;

			case 'y':
				interactive = false;
				break;

			default:
				PrintUsageAndExit(true);
				break;
		}
	}

	// no remaining arguments
	if (argc != optind)
		PrintUsageAndExit(true);

	PackageManager packageManager(location, interactive);
	packageManager.SetDebugLevel(fCommonOptions.DebugLevel());
	packageManager.Autoremove();

	return 0;
}
