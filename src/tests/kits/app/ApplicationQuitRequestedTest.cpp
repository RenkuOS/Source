/*
 * Copyright 2002-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <Message.h>
#include <OS.h>
#include <Application.h>
#include <Handler.h>
#include <Looper.h>
#include <String.h>

#include "ApplicationTest.h"
#include "common/AppRunner.h"

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


class ApplicationQuitRequestedTest : public ApplicationTest {
	CPPUNIT_TEST_SUITE(ApplicationQuitRequestedTest);
	CPPUNIT_TEST(Application_QuitRequested_FirstReturnsFalseThenTrue);
	CPPUNIT_TEST_SUITE_END();

public:
	void Application_QuitRequested_FirstReturnsFalseThenTrue()
	{
		BApplication app("application/x-vnd.obos-app-quit-requested-test");
		const BString output =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n"
			"BApplication::~BApplication()\n";
		// run the app
		AppRunner runner;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.Run("AppQuitRequestedTestApp1"));
		runner.WaitFor(false);
		// get the output and compare the result
		_CheckOutput(runner, output);
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(ApplicationQuitRequestedTest, getTestSuiteName());
