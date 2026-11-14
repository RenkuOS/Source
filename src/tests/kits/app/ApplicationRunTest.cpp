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


class ApplicationRunTest : public ApplicationTest {
	CPPUNIT_TEST_SUITE(ApplicationRunTest);
	CPPUNIT_TEST(MultipleLaunchArgvOnlyWithoutArgs_Run_Succeeds);
	CPPUNIT_TEST(MultipleLaunchArgvOnlyWithArgs_Run_Succeeds);
	CPPUNIT_TEST(MultipleLaunchWithoutArgs_Run_Succeeds);
	CPPUNIT_TEST(MultipleLaunchWithArgs_Run_Succeeds);
	CPPUNIT_TEST(SingleLaunchArgvOnlyWithoutArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(SingleLaunchArgvOnlyWithArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(SingleLaunchWithoutArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(SingleLaunchWithArgs_Run_Relaunches);
	CPPUNIT_TEST(SingleLaunchArgvOnlySameSignatureWithoutArgs_Run_Succeeds);
	CPPUNIT_TEST(SingleLaunchArgvOnlySameSignatureWithArgs_Run_Succeeds);
	CPPUNIT_TEST(SingleLaunchSameSignatureWithoutArgs_Run_Succeeds);
	CPPUNIT_TEST(SingleLaunchSameSignatureWithArgs_Run_Succeeds);
	// CPPUNIT_TEST(ExclusiveLaunchArgvOnlyWithoutArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchArgvOnlyWithArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchWithoutArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchWithArgs_Run_Relaunches);
	CPPUNIT_TEST(ExclusiveLaunchArgvOnlySameSignatureWithoutArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchArgvOnlySameSignatureWithArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchSameSignatureWithoutArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchSameSignatureWithArgs_Run_Relaunches);
	CPPUNIT_TEST(ExclusiveLaunchMixedWithArgs_Run_SecondAppQuits);
	CPPUNIT_TEST(ExclusiveLaunchMixedFlipWithArgs_Run_SecondAppQuits);
	CPPUNIT_TEST_SUITE_END();

public:
	/*
	thread_id Run()
	@case 1			launch the app two times: B_MULTIPLE_LAUNCH | B_ARGV_ONLY,
					no command line args, send B_QUIT_REQUESTED to both of
					them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	ReadyToRun(), QuitRequested()
	*/
	void MultipleLaunchArgvOnlyWithoutArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 = output1;
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp1"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp1"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 2			launch the app two times: B_MULTIPLE_LAUNCH | B_ARGV_ONLY,
					command line args, send B_QUIT_REQUESTED to both of
					them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	ArgvReceived(), ReadyToRun(), QuitRequested()
	*/
	void MultipleLaunchArgvOnlyWithArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp1", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp1", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 3			launch the app two times: B_MULTIPLE_LAUNCH,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	ReadyToRun(), QuitRequested()
	*/
	void MultipleLaunchWithoutArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 = output1;
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp2"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp2"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 4			launch the app two times: B_MULTIPLE_LAUNCH,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	ArgvReceived(), ReadyToRun(), QuitRequested()
	*/
	void MultipleLaunchWithArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp2", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp2", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 5			launch the app two times: B_SINGLE_LAUNCH | B_ARGV_ONLY,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void SingleLaunchArgvOnlyWithoutArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp3"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp3"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 6			launch the app two times: B_SINGLE_LAUNCH | B_ARGV_ONLY,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void SingleLaunchArgvOnlyWithArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp3", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp3", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 7			launch the app two times: B_SINGLE_LAUNCH,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void SingleLaunchWithoutArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp4"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp4"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 8			launch the app two times: B_SINGLE_LAUNCH,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), ArgvReceived(),
					QuitRequested()
					second app:	quits
	*/
	void SingleLaunchWithArgs_Run_Relaunches()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp4", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp4", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 9			launch two apps with the same signature:
					B_SINGLE_LAUNCH | B_ARGV_ONLY,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	ReadyToRun(), QuitRequested()
	*/
	void SingleLaunchArgvOnlySameSignatureWithoutArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 = output1;
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp3"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp3a"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 10		launch two apps with the same signature:
					B_SINGLE_LAUNCH | B_ARGV_ONLY,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	ArgvReceived(), ReadyToRun(), QuitRequested()
	*/
	void SingleLaunchArgvOnlySameSignatureWithArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp3", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp3a", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 11		launch two apps with the same signature:
					B_SINGLE_LAUNCH,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	ReadyToRun(), QuitRequested()
	*/
	void SingleLaunchSameSignatureWithoutArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 = output1;
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp4"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp4a"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 12		launch two apps with the same signature:
					B_SINGLE_LAUNCH,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	ArgvReceived(), ReadyToRun(), QuitRequested()
	*/
	void SingleLaunchSameSignatureWithArgs_Run_Succeeds()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp4", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp4a", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 13		launch the app two times: B_EXCLUSIVE_LAUNCH | B_ARGV_ONLY,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchArgvOnlyWithoutArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp5"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp5"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 14		launch the app two times: B_EXCLUSIVE_LAUNCH | B_ARGV_ONLY,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchArgvOnlyWithArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp5", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp5", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 15		launch the app two times: B_EXCLUSIVE_LAUNCH,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchWithoutArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp6"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp6"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 16		launch the app two times: B_EXCLUSIVE_LAUNCH,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), ArgvReceived(),
					QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchWithArgs_Run_Relaunches()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp6", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp6", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 17		launch two apps with the same signature:
					B_EXCLUSIVE_LAUNCH | B_ARGV_ONLY,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchArgvOnlySameSignatureWithoutArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp5"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp5a"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 18		launch two apps with the same signature:
					B_EXCLUSIVE_LAUNCH | B_ARGV_ONLY,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchArgvOnlySameSignatureWithArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp5", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp5a", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 19		launch two apps with the same signature: B_EXCLUSIVE_LAUNCH,
					no command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchSameSignatureWithoutArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp6"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp6a"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 20		launch two apps with the same signature: B_EXCLUSIVE_LAUNCH,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), ArgvReceived(),
					QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchSameSignatureWithArgs_Run_Relaunches()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp6", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp6a", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 21		launch the app two times: first: B_EXCLUSIVE_LAUNCH,
					second: B_EXCLUSIVE_LAUNCH | B_ARGV_ONLY,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), ArgvReceived(),
					QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchMixedWithArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::ArgvReceived()\n"
			"args: c d e\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp6", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp5", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}

	/*
	thread_id Run()
	@case 22		launch the app two times:
					first: B_EXCLUSIVE_LAUNCH | B_ARGV_ONLY,
					second: B_EXCLUSIVE_LAUNCH,
					command line args, send B_QUIT_REQUESTED to both of them
	@results		first app:	ArgvReceived(), ReadyToRun(), QuitRequested()
					second app:	quits
	*/
	void ExclusiveLaunchMixedFlipWithArgs_Run_SecondAppQuits()
	{
		BApplication app("application/x-vnd.obos-app-run-test");
		const char *output1 =
			"error: 0\n"
			"InitCheck(): 0\n"
			"BApplication::ArgvReceived()\n"
			"args: a b\n"
			"BApplication::ReadyToRun()\n"
			"BApplication::QuitRequested()\n"
			"BApplication::Run() done: 1\n";
		const char *output2 =
			"error: 80002004\n"
			"InitCheck(): 80002004\n";
		// run the apps
		AppRunner runner1, runner2;
		CPPUNIT_ASSERT_EQUAL(B_OK, runner1.Run("AppRunTestApp5", "a b"));
		CPPUNIT_ASSERT_EQUAL(B_OK, runner2.Run("AppRunTestApp6", "c d e"));
		runner1.WaitFor(true);
		runner2.WaitFor(true);
		// get the outputs and compare the results
		_CheckOutput(runner1, output1);
		_CheckOutput(runner2, output2);
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(ApplicationRunTest, getTestSuiteName());
