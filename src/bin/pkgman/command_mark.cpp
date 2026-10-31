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


static const char* const kAutoShortUsage =
	"  %command% <package> ...\n"
	"    Marks packages as installed only as dependencies.\n";

static const char* const kAutoLongUsage =
	"Usage: %program% %command% <package> ...\n"
	"Marks the specified installed packages as installed only as dependencies\n"
	"of other packages. They are uninstalled along with the last package that\n"
	"needs them, or by \"autoremove\" once no installed package needs them.\n"
	"\n"
	"Options:\n"
	"  -H, --home\n"
	"    Mark packages in the user's home directory. Default is to mark\n"
	"    packages in the system directory.\n"
	"\n";

static const char* const kExplicitShortUsage =
	"  %command% <package> ...\n"
	"    Marks packages as explicitly installed.\n";

static const char* const kExplicitLongUsage =
	"Usage: %program% %command% <package> ...\n"
	"Marks the specified installed packages as explicitly installed, so that\n"
	"they are kept when no other package needs them.\n"
	"\n"
	"Options:\n"
	"  -H, --home\n"
	"    Mark packages in the user's home directory. Default is to mark\n"
	"    packages in the system directory.\n"
	"\n";


DEFINE_COMMAND(MarkAutoCommand, "mark-auto", kAutoShortUsage, kAutoLongUsage,
	COMMAND_CATEGORY_PACKAGES)
DEFINE_COMMAND(MarkExplicitCommand, "mark-explicit", kExplicitShortUsage,
	kExplicitLongUsage, COMMAND_CATEGORY_PACKAGES)


static int
mark_packages(Command& command, CommonOptions& commonOptions, int argc,
	const char* const* argv, bool autoInstalled)
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

	// The remaining arguments are the packages to be marked.
	if (argc < optind + 1)
		command.PrintUsageAndExit(true);

	PackageManager packageManager(location, false);
	packageManager.SetDebugLevel(commonOptions.DebugLevel());
	packageManager.SetAutoInstalled(argv + optind, argc - optind,
		autoInstalled);

	return 0;
}


int
MarkAutoCommand::Execute(int argc, const char* const* argv)
{
	return mark_packages(*this, fCommonOptions, argc, argv, true);
}


int
MarkExplicitCommand::Execute(int argc, const char* const* argv)
{
	return mark_packages(*this, fCommonOptions, argc, argv, false);
}
