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

#include "common/AppRunner.h"
#include "common/PipedAppRunner.h"

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


class ApplicationTest : public CppUnit::TestFixture {
protected:
	// gcc2 cannot deal with method templates
	void _CheckOutput(AppRunner &runner, const BString &expectedResult)
	{
		BString buffer;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.GetOutput(&buffer));
		CPPUNIT_ASSERT_EQUAL(expectedResult, buffer);
	}

	void _CheckOutput(PipedAppRunner &runner, const BString &expectedResult)
	{
		BString buffer;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.GetOutput(&buffer));
		CPPUNIT_ASSERT_EQUAL(expectedResult, buffer);
	}

	void _Test(const char* app, const BString &expectedResult)
	{
		// run the app
		PipedAppRunner runner;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner.Run(app));
		runner.WaitFor();

		_CheckOutput(runner, expectedResult);
	}
};