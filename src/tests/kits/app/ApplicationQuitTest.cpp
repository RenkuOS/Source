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
#include "common/PipedAppRunner.h"

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


class ApplicationQuitTest : public ApplicationTest {
	CPPUNIT_TEST_SUITE(ApplicationQuitTest);
	CPPUNIT_TEST(NotRunning_Quit_DeletesApplication);
	CPPUNIT_TEST(RunningCallFromLooper_Quit_ReturnsFromRun);
	CPPUNIT_TEST(RunningCallFromOtherThread_Quit_ReturnsFromRun);
	CPPUNIT_TEST(RunningNoLock_Quit_PrintsErrorAndReturns);
	CPPUNIT_TEST_SUITE_END();

public:
	void NotRunning_Quit_DeletesApplication()
	{
		BApplication app("application/x-vnd.obos-app-quit-test");
		const BString output =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::~BApplication()\n";
		// run the apps
		AppRunner runner;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.Run("AppQuitTestApp1"));
		runner.WaitFor(false);
		_CheckOutput(runner, output);
	}

	void RunningCallFromLooper_Quit_ReturnsFromRun()
	{
		BApplication app("application/x-vnd.obos-app-quit-test");
		const BString output =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::Run() done: 1\n"
			"BApplication::~BApplication()\n";
		// run the apps
		AppRunner runner;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.Run("AppQuitTestApp2"));
		runner.WaitFor(false);
		_CheckOutput(runner, output);
	}

	void RunningCallFromOtherThread_Quit_ReturnsFromRun()
	{
		BApplication app("application/x-vnd.obos-app-quit-test");
		const BString output =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::Run() done: 1\n"
			"BApplication::~BApplication()\n";
		// run the apps
		AppRunner runner;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.Run("AppQuitTestApp3"));
		runner.WaitFor(false);
		_CheckOutput(runner, output);
	}

	void RunningNoLock_Quit_PrintsErrorAndReturns()
	{
		BApplication app("application/x-vnd.obos-app-quit-test");
		const BString output =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::Run() done: 1\n"
			"BApplication::~BApplication()\n";
		// run the apps
		PipedAppRunner runner;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.Run("AppQuitTestApp4"));
		runner.WaitFor();
		// get the output and compare the result
		BString buffer;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.GetOutput(&buffer));
		// Remove the error message, as it contains the team ID, which we don't
		// know.
		int32 errorIndex = buffer.FindFirst(
			"ERROR - you must Lock the application object before calling Quit()");
		CPPUNIT_ASSERT(errorIndex >= 0);
		int32 errorEnd = buffer.FindFirst('\n', errorIndex);
		CPPUNIT_ASSERT(errorEnd >= 0);
		buffer.Remove(errorIndex, errorEnd - errorIndex + 1);
		CPPUNIT_ASSERT_EQUAL(output, buffer);
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(ApplicationQuitTest, getTestSuiteName());
