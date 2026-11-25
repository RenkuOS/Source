/*
 * Copyright 2002-2026, Haiku Project.
 * Distributed under the terms of the MIT License.
 */


#include <Application.h>
#include <Handler.h>
#include <Looper.h>
#include <Message.h>
#include <OS.h>
#include <String.h>

#include "ApplicationTest.h"

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


class ApplicationConstructorTest : public ApplicationTest {
	CPPUNIT_TEST_SUITE(ApplicationConstructorTest);
	CPPUNIT_TEST(NullSignature_Constructor_PrintsErrorAndQuits);
	CPPUNIT_TEST(InvalidMime_Constructor_PrintsErrorAndQuits);
	CPPUNIT_TEST(WrongSupertype_Constructor_PrintsErrorAndQuits);
	CPPUNIT_TEST(MismatchingSignature_Constructor_WarnsAndSucceeds);
	CPPUNIT_TEST(MatchingSignature_Constructor_Succeeds);
	CPPUNIT_TEST_SUITE_END();

public:
	void NullSignature_Constructor_PrintsErrorAndQuits()
	{
		const BString output1 =
			"bad signature ((null)), must begin with \"application/\" and "
			"can't conflict with existing registered mime types inside "
			"the \"application\" media type.\n";
		const BString output2 =
			"bad signature ((null)), must begin with \"application/\" and "
			"can't conflict with existing registered mime types inside "
			"the \"application\" media type.\n"
			"error: 80000005\n"
			"InitCheck(): 80000005\n";
		_Test("BApplicationTestApp1", output1);
		_Test("BApplicationTestApp1a", output1);
		_Test("BApplicationTestApp1b", output2);
	}

	void InvalidMime_Constructor_PrintsErrorAndQuits()
	{
		const BString output1 =
			"bad signature (no valid MIME string), must begin with "
			"\"application/\" and can't conflict with existing registered "
			"mime types inside the \"application\" media type.\n";
		const BString output2 =
			"bad signature (no valid MIME string), must begin with "
			"\"application/\" and can't conflict with existing registered "
			"mime types inside the \"application\" media type.\n"
			"error: 80000005\n"
			"InitCheck(): 80000005\n";
		_Test("BApplicationTestApp2", output1);
		_Test("BApplicationTestApp2a", output1);
		_Test("BApplicationTestApp2b", output2);
	}

	void WrongSupertype_Constructor_PrintsErrorAndQuits()
	{
		const BString output1 =
			"bad signature (image/gif), must begin with \"application/\" and "
			"can't conflict with existing registered mime types inside "
			"the \"application\" media type.\n";
		const BString output2 =
			"bad signature (image/gif), must begin with \"application/\" and "
			"can't conflict with existing registered mime types inside "
			"the \"application\" media type.\n"
			"error: 80000005\n"
			"InitCheck(): 80000005\n";
		_Test("BApplicationTestApp3", output1);
		_Test("BApplicationTestApp3a", output1);
		_Test("BApplicationTestApp3b", output2);
	}

	void MismatchingSignature_Constructor_WarnsAndSucceeds()
	{
		const BString output1 =
			"Signature in rsrc doesn't match constructor arg. "
			"(application/x-vnd.obos-bapplication-testapp4,"
	#ifndef TEST_R5
			" "
	#endif
			"application/x-vnd.obos-bapplication-testapp4-or-not)\n"
			"InitCheck(): 0\n";
		const BString output2 =
			"Signature in rsrc doesn't match constructor arg. "
			"(application/x-vnd.obos-bapplication-testapp4,"
	#ifndef TEST_R5
			" "
	#endif
			"application/x-vnd.obos-bapplication-testapp4-or-not)\n"
			"error: 0\n"
			"InitCheck(): 0\n";
		_Test("BApplicationTestApp4", output1);
		_Test("BApplicationTestApp4a", output1);
		_Test("BApplicationTestApp4b", output2);
	}

	void MatchingSignature_Constructor_Succeeds()
	{
		const BString output1 = "InitCheck(): 0\n";
		const BString output2 =
			"error: 0\n"
			"InitCheck(): 0\n";
		_Test("BApplicationTestApp5", output1);
		_Test("BApplicationTestApp5a", output1);
		_Test("BApplicationTestApp5b", output2);
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(ApplicationConstructorTest, getTestSuiteName());
