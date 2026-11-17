/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests the arm64 userland debugger support: a BRK stops a debugged thread
	with the pc left at the BRK, the debugger can change the thread's CPU
	state (but not the mode bits of SPSR), single-steps advance one
	instruction each, and without a debugger a BRK raises SIGTRAP with
	TRAP_BRKPT. Every case runs in a forked child. Exits 0 when every case
	passes.
*/


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>
#include <debugger.h>


// Returns 4 when the BRK is skipped with x0 left alone. The debugger sets x0
// to kStartValue at the BRK, so the result is then kStartValue + 4.
extern "C" uint64 brk_sequence();
extern "C" char brk_site[];

asm(
	".text\n"
	".global brk_sequence\n"
	".type brk_sequence, %function\n"
	"brk_sequence:\n"
	"	mov x0, #0\n"
	".global brk_site\n"
	"brk_site:\n"
	"	brk #0\n"
	"	add x0, x0, #1\n"
	"	add x0, x0, #1\n"
	"	add x0, x0, #1\n"
	"	add x0, x0, #1\n"
	"	ret\n"
	".size brk_sequence, . - brk_sequence\n");


static const uint64 kStartValue = 100;
static const int kSteps = 4;
static const uint32 kSpsrModeMask = 0xf;
static int sFailures;


#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("    FAILED: %s (line %d, errno %s)\n", #condition, \
				__LINE__, strerror(errno)); \
			fflush(stdout); \
			return false; \
		} \
	} while (false)


static void
report(const char* name, bool passed)
{
	printf("%s: %s\n", name, passed ? "passed" : "FAILED");
	fflush(stdout);
	if (!passed)
		sFailures++;
}


// #pragma mark - debugger


/*!	Reads debugger messages until one with \a code arrives. Other messages
	(e.g. the team's deletion) are skipped.
*/
static bool
wait_for_message(port_id port, int32 code, debug_debugger_message_data& data)
{
	while (true) {
		int32 messageCode;
		ssize_t size = read_port_etc(port, &messageCode, &data, sizeof(data),
			B_RELATIVE_TIMEOUT, 10000000);
		if (size < 0) {
			printf("    no debugger message %" B_PRId32 ": %s\n", code,
				strerror((status_t)size));
			return false;
		}
		if (messageCode == code)
			return true;
		printf("    skipping debugger message %" B_PRId32 "\n", messageCode);
	}
}


static bool
get_cpu_state(port_id nubPort, thread_id thread, debug_cpu_state& state)
{
	port_id replyPort = create_port(1, "arm64_debugger_test reply");
	CHECK(replyPort >= 0);

	debug_nub_get_cpu_state message;
	message.reply_port = replyPort;
	message.thread = thread;
	CHECK(write_port(nubPort, B_DEBUG_MESSAGE_GET_CPU_STATE, &message,
		sizeof(message)) == B_OK);

	debug_nub_get_cpu_state_reply reply;
	int32 code;
	ssize_t size = read_port(replyPort, &code, &reply, sizeof(reply));
	delete_port(replyPort);
	CHECK(size == sizeof(reply));
	CHECK(reply.error == B_OK);
	state = reply.cpu_state;
	return true;
}


static bool
continue_thread(port_id nubPort, thread_id thread, bool singleStep)
{
	debug_nub_continue_thread message;
	message.thread = thread;
	message.handle_event = B_THREAD_DEBUG_HANDLE_EVENT;
	message.single_step = singleStep;
	CHECK(write_port(nubPort, B_DEBUG_MESSAGE_CONTINUE_THREAD, &message,
		sizeof(message)) == B_OK);
	return true;
}


/*!	Debugs \a child, which calls brk_sequence() once it reads a byte from
	\a goFD.
*/
static bool
debug_child(pid_t child, int goFD)
{
	port_id port = create_port(10, "arm64_debugger_test");
	CHECK(port >= 0);
	port_id nubPort = install_team_debugger(child, port);
	CHECK(nubPort >= 0);
	CHECK(write(goFD, "g", 1) == 1);

	// the pc is left at the BRK
	debug_debugger_message_data data;
	CHECK(wait_for_message(port, B_DEBUGGER_MESSAGE_BREAKPOINT_HIT, data));
	thread_id thread = data.origin.thread;
	debug_cpu_state state = data.breakpoint_hit.cpu_state;
	CHECK(state.elr == (addr_t)brk_site);
	CHECK(state.x[0] == 0);

	// skip the BRK and change x0; the mode bits of SPSR (EL0t) are kept
	state.elr += 4;
	state.x[0] = kStartValue;
	uint32 spsr = state.spsr;
	state.spsr = (spsr & ~kSpsrModeMask) | 0x5;	// EL1h
	debug_nub_set_cpu_state setState;
	setState.thread = thread;
	setState.cpu_state = state;
	CHECK(write_port(nubPort, B_DEBUG_MESSAGE_SET_CPU_STATE, &setState,
		sizeof(setState)) == B_OK);

	CHECK(get_cpu_state(nubPort, thread, state));
	CHECK(state.elr == (addr_t)brk_site + 4);
	CHECK(state.x[0] == kStartValue);
	CHECK((state.spsr & kSpsrModeMask) == (spsr & kSpsrModeMask));

	// each step runs exactly one "add x0, x0, #1"
	for (int i = 1; i <= kSteps; i++) {
		CHECK(continue_thread(nubPort, thread, true));
		CHECK(wait_for_message(port, B_DEBUGGER_MESSAGE_SINGLE_STEP, data));
		CHECK(data.origin.thread == thread);
		CHECK(data.single_step.cpu_state.elr == (addr_t)brk_site + 4 + 4 * i);
		CHECK(data.single_step.cpu_state.x[0] == kStartValue + i);
	}

	CHECK(continue_thread(nubPort, thread, false));
	return true;
}


static void
test_debugger()
{
	int pipeFDs[2];
	if (pipe(pipeFDs) != 0) {
		printf("    cannot create pipe: %s\n", strerror(errno));
		report("breakpoint, set cpu state, single step", false);
		return;
	}

	fflush(stdout);
	pid_t child = fork();
	if (child < 0) {
		printf("    cannot fork: %s\n", strerror(errno));
		report("breakpoint, set cpu state, single step", false);
		return;
	}
	if (child == 0) {
		close(pipeFDs[1]);
		char go;
		if (read(pipeFDs[0], &go, 1) != 1)
			_exit(2);
		_exit(brk_sequence() == kStartValue + kSteps ? 0 : 1);
	}
	close(pipeFDs[0]);

	bool passed = debug_child(child, pipeFDs[1]);
	close(pipeFDs[1]);
	if (!passed)
		kill(child, SIGKILL);

	int status;
	if (waitpid(child, &status, 0) != child) {
		printf("    waitpid failed: %s\n", strerror(errno));
		passed = false;
	} else if (passed && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
		printf("    child status %#x\n", status);
		passed = false;
	}
	report("breakpoint, set cpu state, single step", passed);
}


// #pragma mark - no debugger


static void
sigtrap_handler(int signal, siginfo_t* info, void*)
{
	// the pc is still at the BRK, so returning would trap again
	_exit(signal == SIGTRAP && info->si_code == TRAP_BRKPT
		&& info->si_addr == brk_site ? 0 : 1);
}


static void
test_no_debugger()
{
	fflush(stdout);
	pid_t child = fork();
	if (child < 0) {
		printf("    cannot fork: %s\n", strerror(errno));
		report("SIGTRAP without a debugger", false);
		return;
	}
	if (child == 0) {
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_sigaction = sigtrap_handler;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		sigaction(SIGTRAP, &action, NULL);
		brk_sequence();
		_exit(3);
	}

	int status = 0;
	bool passed = waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0;
	if (!passed)
		printf("    child status %#x\n", status);
	report("SIGTRAP without a debugger", passed);
}


// #pragma mark -


int
main()
{
	test_debugger();
	test_no_debugger();

	printf("%d failure(s)\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
