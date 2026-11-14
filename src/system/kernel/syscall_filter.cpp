/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Per-team syscall restriction.

	restrict_syscalls() lets a team give up groups of syscalls, irreversibly,
	for itself and every team it creates afterwards. A team that has given up
	anything gets THREAD_FLAGS_SYSCALL_FILTERED on all of its threads, which
	makes the syscall entry call syscall_filter_check() before every syscall.

	Groups are named, not numbered: syscall numbers follow the order of
	<syscalls.h> and change between builds. The table below names the
	syscalls of each group. A syscall it does not name -- including every one
	added after it was written -- is in B_SYSCALL_GROUP_SYSTEM, so a filtered
	team does not gain syscalls by accident.

	Some syscalls take the ID of a thread, semaphore, area or team. In
	B_SYSCALL_GROUP_BASIC they may only name the caller's own; naming another
	team's needs B_SYSCALL_GROUP_SYSTEM. Opening with write access needs
	B_SYSCALL_GROUP_FILE_WRITE, stat()ing a path B_SYSCALL_GROUP_FILE_READ.
	Generic syscalls are BASIC only for getentropy(); every other subsystem
	needs SYSTEM (checked by _user_generic_syscall(), on its own copy of the
	subsystem name).
*/


#include <syscall_filter.h>

#include <arch_config.h>
#include <fcntl.h>
#include <new>
#include <string.h>
#include <sys/ioctl.h>

#include <ksyscalls.h>
#include <random_defs.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>


//#define TRACE_SYSCALL_FILTER
#ifdef TRACE_SYSCALL_FILTER
#	define TRACE(x...) dprintf("syscall_filter: " x)
#else
#	define TRACE(x...) ;
#endif


// The syscall entry has to call syscall_filter_check() for this to mean
// anything. x86_64 does so in its native and compat entries, and the
// architectures that dispatch through syscall_dispatcher() with 64 bit
// argument slots do so there. Elsewhere restrict_syscalls() fails.
#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv)
#	define SYSCALL_FILTER_SUPPORTED 1
#endif


namespace {


// Never filtered: without them a team could not even exit.
static const uint16 kGroupAlways = 0;

enum {
	kCheckNone = 0,
	kCheckThread,		// args[0] is a thread_id
	kCheckWaitThread,	// args[0] is a thread_id, which may have exited
	kCheckTeam,			// args[0] is a team_id (B_CURRENT_TEAM allowed)
	kCheckSem,			// args[0] is a sem_id
	kCheckTwoSems,		// args[0] and args[1] are sem_ids
	kCheckArea,			// args[0] is an area_id
	kCheckSignal,		// args[0] is the target of send_signal()
	kCheckOpenMode2,	// args[2] is an open mode
	kCheckOpenMode3,	// args[3] is an open mode
	kCheckOpenMode4,	// args[4] is an open mode
	kCheckStatPath,		// args[1] is a path, or NULL for the descriptor
	kCheckIoctl,		// args[1] is an ioctl op
};

struct SyscallGroupEntry {
	const char*	name;
	uint16		group;
	uint8		check;
};

#define ALWAYS		kGroupAlways
#define BASIC		B_SYSCALL_GROUP_BASIC
#define FILE_READ	B_SYSCALL_GROUP_FILE_READ
#define FILE_WRITE	B_SYSCALL_GROUP_FILE_WRITE
#define SOCKET		B_SYSCALL_GROUP_SOCKET
#define PROCESS		B_SYSCALL_GROUP_PROCESS
#define PORT		B_SYSCALL_GROUP_PORT
#define AREA_SHARE	B_SYSCALL_GROUP_AREA_SHARE
#define DEBUG		B_SYSCALL_GROUP_DEBUG

static const SyscallGroupEntry kSyscallGroups[] = {
	{ "_kern_exit_thread", ALWAYS, kCheckNone },
	{ "_kern_exit_team", ALWAYS, kCheckNone },
	{ "_kern_restore_signal_frame", ALWAYS, kCheckNone },
	{ "_kern_restrict_syscalls", ALWAYS, kCheckNone },

	// BASIC: atomics and user mutexes
#ifdef ATOMIC_FUNCS_ARE_SYSCALLS
	{ "_kern_atomic_add", BASIC, kCheckNone },
	{ "_kern_atomic_and", BASIC, kCheckNone },
	{ "_kern_atomic_get", BASIC, kCheckNone },
	{ "_kern_atomic_get_and_set", BASIC, kCheckNone },
	{ "_kern_atomic_or", BASIC, kCheckNone },
	{ "_kern_atomic_set", BASIC, kCheckNone },
	{ "_kern_atomic_test_and_set", BASIC, kCheckNone },
#endif
#ifdef ATOMIC64_FUNCS_ARE_SYSCALLS
	{ "_kern_atomic_add64", BASIC, kCheckNone },
	{ "_kern_atomic_and64", BASIC, kCheckNone },
	{ "_kern_atomic_get64", BASIC, kCheckNone },
	{ "_kern_atomic_get_and_set64", BASIC, kCheckNone },
	{ "_kern_atomic_or64", BASIC, kCheckNone },
	{ "_kern_atomic_set64", BASIC, kCheckNone },
	{ "_kern_atomic_test_and_set64", BASIC, kCheckNone },
#endif
	{ "_kern_mutex_lock", BASIC, kCheckNone },
	{ "_kern_mutex_unblock", BASIC, kCheckNone },
	{ "_kern_mutex_switch_lock", BASIC, kCheckNone },
	{ "_kern_mutex_sem_acquire", BASIC, kCheckNone },
	{ "_kern_mutex_sem_release", BASIC, kCheckNone },

	// BASIC: the team's own semaphores
	{ "_kern_create_sem", BASIC, kCheckNone },
	{ "_kern_delete_sem", BASIC, kCheckSem },
	{ "_kern_acquire_sem", BASIC, kCheckSem },
	{ "_kern_acquire_sem_etc", BASIC, kCheckSem },
	{ "_kern_release_sem", BASIC, kCheckSem },
	{ "_kern_release_sem_etc", BASIC, kCheckSem },
	{ "_kern_switch_sem", BASIC, kCheckTwoSems },
	{ "_kern_switch_sem_etc", BASIC, kCheckTwoSems },
	{ "_kern_get_sem_count", BASIC, kCheckSem },
	{ "_kern_get_sem_info", BASIC, kCheckSem },

	// BASIC: memory and the team's own areas
	{ "_kern_create_area", BASIC, kCheckNone },
	{ "_kern_delete_area", BASIC, kCheckArea },
	{ "_kern_resize_area", BASIC, kCheckArea },
	{ "_kern_set_area_protection", BASIC, kCheckArea },
	{ "_kern_get_area_info", BASIC, kCheckArea },
	{ "_kern_get_next_area_info", BASIC, kCheckTeam },
	{ "_kern_area_for", BASIC, kCheckNone },
	{ "_kern_reserve_address_range", BASIC, kCheckNone },
	{ "_kern_unreserve_address_range", BASIC, kCheckNone },
	{ "_kern_map_file", BASIC, kCheckNone },
	{ "_kern_unmap_memory", BASIC, kCheckNone },
	{ "_kern_set_memory_protection", BASIC, kCheckNone },
	{ "_kern_sync_memory", BASIC, kCheckNone },
	{ "_kern_memory_advice", BASIC, kCheckNone },
	{ "_kern_get_memory_properties", BASIC, kCheckTeam },
	{ "_kern_mlock", BASIC, kCheckNone },
	{ "_kern_munlock", BASIC, kCheckNone },

	// BASIC: the team's own threads
	{ "_kern_spawn_thread", BASIC, kCheckNone },
	{ "_kern_kill_thread", BASIC, kCheckThread },
	{ "_kern_suspend_thread", BASIC, kCheckThread },
	{ "_kern_resume_thread", BASIC, kCheckThread },
	{ "_kern_rename_thread", BASIC, kCheckThread },
	{ "_kern_set_thread_priority", BASIC, kCheckThread },
	{ "_kern_wait_for_thread_etc", BASIC, kCheckWaitThread },
	{ "_kern_get_thread_info", BASIC, kCheckThread },
	{ "_kern_get_next_thread_info", BASIC, kCheckTeam },
	{ "_kern_get_thread_affinity", BASIC, kCheckThread },
	{ "_kern_set_thread_affinity", BASIC, kCheckThread },
	{ "_kern_cancel_thread", BASIC, kCheckThread },
	{ "_kern_send_data", BASIC, kCheckThread },
	{ "_kern_receive_data", BASIC, kCheckNone },
	{ "_kern_has_data", BASIC, kCheckThread },
	{ "_kern_unblock_thread", BASIC, kCheckThread },
	{ "_kern_block_thread", BASIC, kCheckNone },
	{ "_kern_unblock_threads", BASIC, kCheckNone },
		// takes an array; a stray wake-up only makes the woken thread
		// recheck its condition
	{ "_kern_thread_yield", BASIC, kCheckNone },
	{ "_kern_snooze_etc", BASIC, kCheckNone },

	// BASIC: the team itself
	{ "_kern_get_current_team", BASIC, kCheckNone },
	{ "_kern_get_team_info", BASIC, kCheckTeam },
	{ "_kern_get_team_usage_info", BASIC, kCheckTeam },
	{ "_kern_get_extended_team_info", BASIC, kCheckTeam },
	{ "_kern_process_info", BASIC, kCheckTeam },
	{ "_kern_setpgid", BASIC, kCheckTeam },
	{ "_kern_setsid", BASIC, kCheckNone },
	{ "_kern_getrlimit", BASIC, kCheckNone },
	{ "_kern_setrlimit", BASIC, kCheckNone },
	{ "_kern_getgroups", BASIC, kCheckNone },
	{ "_kern_getresuid", BASIC, kCheckNone },
	{ "_kern_getresgid", BASIC, kCheckNone },
	{ "_kern_setresuid", BASIC, kCheckNone },
	{ "_kern_setresgid", BASIC, kCheckNone },
	{ "_kern_setgroups", BASIC, kCheckNone },
		// credentials are checked by the syscalls themselves
	{ "_kern_disable_debugger", BASIC, kCheckNone },
	{ "_kern_register_image", BASIC, kCheckNone },
	{ "_kern_unregister_image", BASIC, kCheckNone },
	{ "_kern_image_relocated", BASIC, kCheckNone },
	{ "_kern_loading_app_failed", BASIC, kCheckNone },
	{ "_kern_get_image_info", BASIC, kCheckNone },
	{ "_kern_get_next_image_info", BASIC, kCheckTeam },

	// BASIC: signals, to itself
	{ "_kern_send_signal", BASIC, kCheckSignal },
	{ "_kern_set_signal_mask", BASIC, kCheckNone },
	{ "_kern_sigaction", BASIC, kCheckNone },
	{ "_kern_sigwait", BASIC, kCheckNone },
	{ "_kern_sigsuspend", BASIC, kCheckNone },
	{ "_kern_sigpending", BASIC, kCheckNone },
	{ "_kern_set_signal_stack", BASIC, kCheckNone },

	// BASIC: time and system information
	{ "_kern_system_time", BASIC, kCheckNone },
	{ "_kern_get_clock", BASIC, kCheckNone },
	{ "_kern_get_cpuclockid", BASIC, kCheckNone },
	{ "_kern_get_timezone", BASIC, kCheckNone },
	{ "_kern_get_real_time_clock_is_gmt", BASIC, kCheckNone },
	{ "_kern_create_timer", BASIC, kCheckNone },
	{ "_kern_delete_timer", BASIC, kCheckNone },
	{ "_kern_get_timer", BASIC, kCheckNone },
	{ "_kern_set_timer", BASIC, kCheckNone },
		// timers belong to the team, which the syscalls check
	{ "_kern_get_system_info", BASIC, kCheckNone },
	{ "_kern_get_cpu_info", BASIC, kCheckNone },
	{ "_kern_get_cpu_topology_info", BASIC, kCheckNone },
	{ "_kern_get_cpu", BASIC, kCheckNone },
	{ "_kern_get_cpuid", BASIC, kCheckNone },
	{ "_kern_cpu_enabled", BASIC, kCheckNone },
	{ "_kern_get_loadavg", BASIC, kCheckNone },
	{ "_kern_get_safemode_option", BASIC, kCheckNone },
	{ "_kern_is_computer_on", BASIC, kCheckNone },
	{ "_kern_get_scheduler_mode", BASIC, kCheckNone },
	{ "_kern_estimate_max_scheduling_latency", BASIC, kCheckNone },
	{ "_kern_generic_syscall", BASIC, kCheckNone },
		// getentropy(); syscall_filter_allows_generic_syscall() denies
		// every other subsystem

	// BASIC: descriptors the team already has
	{ "_kern_read", BASIC, kCheckNone },
	{ "_kern_readv", BASIC, kCheckNone },
	{ "_kern_write", BASIC, kCheckNone },
	{ "_kern_writev", BASIC, kCheckNone },
	{ "_kern_seek", BASIC, kCheckNone },
	{ "_kern_close", BASIC, kCheckNone },
	{ "_kern_close_range", BASIC, kCheckNone },
	{ "_kern_dup", BASIC, kCheckNone },
	{ "_kern_dup2", BASIC, kCheckNone },
	{ "_kern_fcntl", BASIC, kCheckNone },
	{ "_kern_flock", BASIC, kCheckNone },
	{ "_kern_fsync", BASIC, kCheckNone },
	{ "_kern_ioctl", BASIC, kCheckIoctl },
	{ "_kern_read_stat", BASIC, kCheckStatPath },
	{ "_kern_get_next_fd_info", BASIC, kCheckTeam },
	{ "_kern_poll", BASIC, kCheckNone },
	{ "_kern_select", BASIC, kCheckNone },
	{ "_kern_wait_for_objects", BASIC, kCheckNone },
	{ "_kern_event_queue_create", BASIC, kCheckNone },
	{ "_kern_event_queue_select", BASIC, kCheckNone },
	{ "_kern_event_queue_wait", BASIC, kCheckNone },
	{ "_kern_create_pipe", BASIC, kCheckNone },
	{ "_kern_socketpair", BASIC, kCheckNone },
		// a connected pair reaches nothing outside the team
	{ "_kern_recv", BASIC, kCheckNone },
	{ "_kern_recvfrom", BASIC, kCheckNone },
	{ "_kern_recvmsg", BASIC, kCheckNone },
	{ "_kern_send", BASIC, kCheckNone },
	{ "_kern_sendto", BASIC, kCheckNone },
	{ "_kern_sendmsg", BASIC, kCheckNone },
	{ "_kern_getsockopt", BASIC, kCheckNone },
	{ "_kern_setsockopt", BASIC, kCheckNone },
	{ "_kern_getsockname", BASIC, kCheckNone },
	{ "_kern_getpeername", BASIC, kCheckNone },
	{ "_kern_shutdown_socket", BASIC, kCheckNone },
	{ "_kern_sockatmark", BASIC, kCheckNone },

	// FILE_READ
	{ "_kern_open", FILE_READ, kCheckOpenMode2 },
	{ "_kern_open_entry_ref", FILE_READ, kCheckOpenMode3 },
	{ "_kern_open_attr", FILE_READ, kCheckOpenMode4 },
	{ "_kern_open_dir", FILE_READ, kCheckNone },
	{ "_kern_open_dir_entry_ref", FILE_READ, kCheckNone },
	{ "_kern_open_parent_dir", FILE_READ, kCheckNone },
	{ "_kern_read_dir", FILE_READ, kCheckNone },
	{ "_kern_rewind_dir", FILE_READ, kCheckNone },
	{ "_kern_access", FILE_READ, kCheckNone },
	{ "_kern_read_link", FILE_READ, kCheckNone },
	{ "_kern_entry_ref_to_path", FILE_READ, kCheckNone },
	{ "_kern_normalize_path", FILE_READ, kCheckNone },
	{ "_kern_getcwd", FILE_READ, kCheckNone },
	{ "_kern_setcwd", FILE_READ, kCheckNone },
	{ "_kern_read_attr", FILE_READ, kCheckNone },
	{ "_kern_open_attr_dir", FILE_READ, kCheckNone },
	{ "_kern_stat_attr", FILE_READ, kCheckNone },
	{ "_kern_read_fs_info", FILE_READ, kCheckNone },
	{ "_kern_open_query", FILE_READ, kCheckNone },
	{ "_kern_open_index_dir", FILE_READ, kCheckNone },
	{ "_kern_read_index_stat", FILE_READ, kCheckNone },

	// FILE_WRITE
	{ "_kern_create_dir", FILE_WRITE, kCheckNone },
	{ "_kern_create_dir_entry_ref", FILE_WRITE, kCheckNone },
	{ "_kern_create_fifo", FILE_WRITE, kCheckNone },
	{ "_kern_create_link", FILE_WRITE, kCheckNone },
	{ "_kern_create_symlink", FILE_WRITE, kCheckNone },
	{ "_kern_remove_dir", FILE_WRITE, kCheckNone },
	{ "_kern_unlink", FILE_WRITE, kCheckNone },
	{ "_kern_rename", FILE_WRITE, kCheckNone },
	{ "_kern_write_attr", FILE_WRITE, kCheckNone },
	{ "_kern_remove_attr", FILE_WRITE, kCheckNone },
	{ "_kern_rename_attr", FILE_WRITE, kCheckNone },
	{ "_kern_write_stat", FILE_WRITE, kCheckNone },
	{ "_kern_preallocate", FILE_WRITE, kCheckNone },
	{ "_kern_lock_node", FILE_WRITE, kCheckNone },
	{ "_kern_unlock_node", FILE_WRITE, kCheckNone },
	{ "_kern_create_index", FILE_WRITE, kCheckNone },
	{ "_kern_remove_index", FILE_WRITE, kCheckNone },
	{ "_kern_write_fs_info", FILE_WRITE, kCheckNone },
	{ "_kern_sync", FILE_WRITE, kCheckNone },

	// SOCKET
	{ "_kern_socket", SOCKET, kCheckNone },
	{ "_kern_bind", SOCKET, kCheckNone },
	{ "_kern_listen", SOCKET, kCheckNone },
	{ "_kern_accept", SOCKET, kCheckNone },
	{ "_kern_connect", SOCKET, kCheckNone },

	// PROCESS
	{ "_kern_fork", PROCESS, kCheckNone },
	{ "_kern_exec", PROCESS, kCheckNone },
	{ "_kern_load_image", PROCESS, kCheckNone },
	{ "_kern_wait_for_child", PROCESS, kCheckNone },
	{ "_kern_wait_for_team", PROCESS, kCheckNone },

	// PORT: ports, and the monitoring syscalls that deliver to a port
	{ "_kern_create_port", PORT, kCheckNone },
	{ "_kern_close_port", PORT, kCheckNone },
	{ "_kern_delete_port", PORT, kCheckNone },
	{ "_kern_find_port", PORT, kCheckNone },
	{ "_kern_get_port_info", PORT, kCheckNone },
	{ "_kern_get_next_port_info", PORT, kCheckNone },
	{ "_kern_port_buffer_size_etc", PORT, kCheckNone },
	{ "_kern_port_count", PORT, kCheckNone },
	{ "_kern_set_port_owner", PORT, kCheckNone },
	{ "_kern_read_port_etc", PORT, kCheckNone },
	{ "_kern_write_port_etc", PORT, kCheckNone },
	{ "_kern_writev_port_etc", PORT, kCheckNone },
	{ "_kern_get_port_message_info_etc", PORT, kCheckNone },
	{ "_kern_start_watching", PORT, kCheckNone },
	{ "_kern_stop_watching", PORT, kCheckNone },
	{ "_kern_stop_notifying", PORT, kCheckNone },
	{ "_kern_start_watching_system", PORT, kCheckNone },
	{ "_kern_stop_watching_system", PORT, kCheckNone },
	{ "_kern_start_watching_disks", PORT, kCheckNone },
	{ "_kern_stop_watching_disks", PORT, kCheckNone },
	{ "_kern_register_messaging_service", PORT, kCheckNone },
	{ "_kern_unregister_messaging_service", PORT, kCheckNone },

	// AREA_SHARE
	{ "_kern_clone_area", AREA_SHARE, kCheckNone },
	{ "_kern_transfer_area", AREA_SHARE, kCheckNone },

	// DEBUG
	{ "_kern_debugger", DEBUG, kCheckNone },
	{ "_kern_install_default_debugger", DEBUG, kCheckNone },
	{ "_kern_install_team_debugger", DEBUG, kCheckNone },
	{ "_kern_remove_team_debugger", DEBUG, kCheckNone },
	{ "_kern_debug_thread", DEBUG, kCheckNone },
	{ "_kern_wait_for_debugger", DEBUG, kCheckNone },
	{ "_kern_set_debugger_breakpoint", DEBUG, kCheckNone },
	{ "_kern_clear_debugger_breakpoint", DEBUG, kCheckNone },

	// Everything else is B_SYSCALL_GROUP_SYSTEM.
};

#undef ALWAYS
#undef BASIC
#undef FILE_READ
#undef FILE_WRITE
#undef SOCKET
#undef PROCESS
#undef PORT
#undef AREA_SHARE
#undef DEBUG


static uint16* sSyscallGroup;
static uint8* sSyscallCheck;


enum {
	kForeign,
	kOwn,
	kChild		// a thread of a team the caller created
};


/*!	Whether  team holds the death record of thread  id: of a child team's
	main thread, or of one of its own threads. wait_for_thread() on a thread
	that has exited reads only these records, so a sandboxed team can still
	collect the exit status of a child that finished first.
	The caller must hold the team's lock.
*/
static int
dead_thread_relation_locked(Team* team, thread_id id)
{
	for (job_control_entry* entry = team->dead_children.entries.First();
			entry != NULL;
			entry = team->dead_children.entries.GetNext(entry)) {
		if (entry->thread == id)
			return kChild;
	}

	for (thread_death_entry* entry = team->dead_threads.First();
			entry != NULL; entry = team->dead_threads.GetNext(entry)) {
		if (entry->thread == id)
			return kOwn;
	}

	return kForeign;
}


/*!	How thread  id relates to  team. With  deathRecords, a thread that has
	exited still counts as the team's or its child's if  team holds its
	death record (for wait_for_thread()).
*/
static int
thread_relation(Team* team, thread_id id, bool deathRecords = false)
{
	// Taken before the team lock, so that the last reference is never put
	// while holding it.
	Thread* thread = Thread::Get(id);
	BReference<Thread> reference(thread, true);

	// An exiting thread moves into the kernel team, and its death record
	// into its team (or, for a main thread, the parent), with that team
	// locked, before it disappears from the thread hash. Under the caller's
	// lock, a thread of its own or of a child is therefore either still
	// there or already recorded, never neither.
	TeamLocker teamLocker(team);

	if (thread != NULL) {
		InterruptsReadSpinLocker threadTeamLocker(thread->team_lock);
		// The thread's team cannot go away while the thread is in it.
		if (thread->team == team)
			return kOwn;
		// The parent link only changes when the parent dies, and the
		// caller is alive.
		if (thread->team->parent == team)
			return kChild;
	}

	// Gone, or on its way out in the kernel team. Not merely "unknown, so
	// allowed": an ID not in use yet may be another team's thread by the
	// time the syscall runs.
	return deathRecords ? dead_thread_relation_locked(team, id) : kForeign;
}


/*!	The group a syscall naming thread  id needs:  group for the caller's
	own threads, PROCESS for its children's (resume_thread() and
	wait_for_thread() are how load_image()'s caller runs its child), SYSTEM
	for anyone else's.
*/
static uint32
thread_group(Team* team, thread_id id, uint32 group,
	bool deathRecords = false)
{
	switch (thread_relation(team, id, deathRecords)) {
		case kOwn:
			return group;
		case kChild:
			return B_SYSCALL_GROUP_PROCESS;
		default:
			return B_SYSCALL_GROUP_SYSTEM;
	}
}


static bool
is_own_team(Team* team, team_id id)
{
	return id == B_CURRENT_TEAM || id == team->id;
}


static bool
is_own_sem(Team* team, sem_id id)
{
	sem_info info;
	return _get_sem_info(id, &info, sizeof(info)) == B_OK
		&& info.team == team->id;
}


static bool
is_own_area(Team* team, area_id id)
{
	area_info info;
	return _get_area_info(id, &info, sizeof(info)) == B_OK
		&& info.team == team->id;
}


static bool
is_write_open_mode(int openMode)
{
	return (openMode & O_ACCMODE) != O_RDONLY
		|| (openMode & (O_CREAT | O_TRUNC)) != 0;
}


}	// namespace


status_t
syscall_filter_init(void)
{
	sSyscallGroup = new(std::nothrow) uint16[kSyscallCount];
	sSyscallCheck = new(std::nothrow) uint8[kSyscallCount];
	if (sSyscallGroup == NULL || sSyscallCheck == NULL)
		panic("syscall_filter_init(): out of memory");

	for (int i = 0; i < kSyscallCount; i++) {
		sSyscallGroup[i] = B_SYSCALL_GROUP_SYSTEM;
		sSyscallCheck[i] = kCheckNone;
	}

	for (size_t i = 0; i < B_COUNT_OF(kSyscallGroups); i++) {
		const SyscallGroupEntry& entry = kSyscallGroups[i];
		int index = 0;
		while (index < kSyscallCount
			&& strcmp(kExtendedSyscallInfos[index].name, entry.name) != 0) {
			index++;
		}

		if (index == kSyscallCount) {
			// A syscall was renamed or removed: it stays in SYSTEM, which
			// denies more, never less.
			dprintf("syscall_filter: no syscall named %s\n", entry.name);
			continue;
		}

		sSyscallGroup[index] = entry.group;
		sSyscallCheck[index] = entry.check;
	}

	return B_OK;
}


status_t
syscall_filter_check(uint32 syscall, const uint64* args)
{
	if (syscall >= (uint32)kSyscallCount)
		return B_NOT_ALLOWED;

	Team* team = thread_get_current_thread()->team;
	uint32 allowed = (uint32)atomic_get(&team->syscall_groups);
	uint32 group = sSyscallGroup[syscall];

	switch (sSyscallCheck[syscall]) {
		case kCheckThread:
			group = thread_group(team, (thread_id)args[0], group);
			break;
		case kCheckWaitThread:
			group = thread_group(team, (thread_id)args[0], group, true);
			break;
		case kCheckTeam:
			if (!is_own_team(team, (team_id)args[0]))
				group = B_SYSCALL_GROUP_SYSTEM;
			break;
		case kCheckSem:
			if (!is_own_sem(team, (sem_id)args[0]))
				group = B_SYSCALL_GROUP_SYSTEM;
			break;
		case kCheckTwoSems:
			// switch_sem(): a negative semaphore to release means none
			if (((sem_id)args[0] >= 0 && !is_own_sem(team, (sem_id)args[0]))
				|| !is_own_sem(team, (sem_id)args[1])) {
				group = B_SYSCALL_GROUP_SYSTEM;
			}
			break;
		case kCheckArea:
			if (!is_own_area(team, (area_id)args[0]))
				group = B_SYSCALL_GROUP_SYSTEM;
			break;
		case kCheckSignal:
		{
			// The team itself or one of its threads; its children (whose
			// team IDs are their main threads' IDs) with PROCESS. Process
			// groups (<= 0) and everyone else need SYSTEM.
			int32 target = (int32)args[0];
			if (target <= 0)
				group = B_SYSCALL_GROUP_SYSTEM;
			else if (target != team->id)
				group = thread_group(team, (thread_id)target, group);
			break;
		}
		case kCheckOpenMode2:
			if (is_write_open_mode((int)args[2]))
				group = B_SYSCALL_GROUP_FILE_WRITE;
			break;
		case kCheckOpenMode3:
			if (is_write_open_mode((int)args[3]))
				group = B_SYSCALL_GROUP_FILE_WRITE;
			break;
		case kCheckOpenMode4:
			if (is_write_open_mode((int)args[4]))
				group = B_SYSCALL_GROUP_FILE_WRITE;
			break;
		case kCheckStatPath:
			if (args[1] != 0)
				group = B_SYSCALL_GROUP_FILE_READ;
			break;
		case kCheckIoctl:
		{
			// Socket and file ioctls that do not reach a device driver.
			uint32 op = (uint32)args[1];
			if (op != FIONBIO && op != FIONREAD)
				group = B_SYSCALL_GROUP_SYSTEM;
			break;
		}
	}

	if (group == kGroupAlways || (group & allowed) != 0)
		return B_OK;

	TRACE("team %" B_PRId32 ": denied %s\n", team->id,
		kExtendedSyscallInfos[syscall].name);
	return B_NOT_ALLOWED;
}


bool
syscall_filter_allows_generic_syscall(const char* subsystem, uint32 function)
{
	Team* team = thread_get_current_thread()->team;
	uint32 allowed = (uint32)atomic_get(&team->syscall_groups);
	if ((allowed & B_SYSCALL_GROUP_SYSTEM) != 0)
		return true;

	// getentropy(): the stack protector, mktemp() and every random number
	// generator need it, and it reaches nothing outside the team.
	if ((allowed & B_SYSCALL_GROUP_BASIC) != 0
		&& strcmp(subsystem, RANDOM_SYSCALLS) == 0
		&& function == RANDOM_GET_ENTROPY) {
		return true;
	}

	TRACE("team %" B_PRId32 ": denied generic syscall %s %" B_PRIu32 "\n",
		team->id, subsystem, function);
	return false;
}


status_t
_user_restrict_syscalls(uint32 allowedGroups)
{
#ifndef SYSCALL_FILTER_SUPPORTED
	return B_NOT_SUPPORTED;
#else
	if ((allowedGroups & ~(uint32)B_SYSCALL_GROUP_ALL) != 0)
		return B_BAD_VALUE;

	Thread* currentThread = thread_get_current_thread();
	Team* team = currentThread->team;

	// The thread list is protected by the team lock, and thread creation
	// sets the flag for new threads under the same lock. (Not a TeamLocker:
	// its NULL check makes GCC warn about the atomics below.)
	team->Lock();

	int32 groups = atomic_get(&team->syscall_groups) & (int32)allowedGroups;
	atomic_set(&team->syscall_groups, groups);

	if (groups != B_SYSCALL_GROUP_ALL) {
		for (Thread* thread = team->thread_list.First(); thread != NULL;
				thread = team->thread_list.GetNext(thread)) {
			atomic_or(&thread->flags, THREAD_FLAGS_SYSCALL_FILTERED);
		}
	}

	team->Unlock();
	return B_OK;
#endif
}
