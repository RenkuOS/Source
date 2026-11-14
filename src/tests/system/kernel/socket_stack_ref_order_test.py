#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the production accept()/socketpair() stack-interface reference
ordering: every published socket FD must already own its net stack interface
module reference, and the failure paths must dispose of every socket and FD."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[4])
source = (root / 'src/system/kernel/fs/socket.cpp').read_text()


def extract(start_marker, body_marker=None):
    index = source.index(body_marker or start_marker)
    start = source.rindex(start_marker, 0, index + 1)
    return source[start:source.index('\n}', index) + 2]


interface = extract('static net_stack_interface_module_info* sStackInterface',
                    'put_stack_interface_module()')
accept = extract('static int\n', 'common_accept(int fd')
socketpair = extract('static status_t\n', 'common_socketpair(int family')

prelude = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int status_t;
typedef int int32;
typedef unsigned int socklen_t;

#define B_OK 0
#define B_NO_MEMORY (-1)
#define B_UNSUPPORTED (-2)
#define B_BAD_VALUE (-3)
#define B_FILE_ERROR (-4)

#define KDEBUG 1
#define NET_STACK_INTERFACE_MODULE_NAME "network/stack/v1"
#define SOCK_CLOEXEC 0x1000
#define SOCK_NONBLOCK 0x2000
#define SOCK_CLOFORK 0x4000

static int32 atomic_add(int32* value, int32 delta)
{
    int32 old = *value;
    *value += delta;
    // The consumer count may never drift negative: that is the put() of a
    // reference the published FD never acquired.
    assert(*value >= 0);
    return old;
}
static int32 atomic_get(int32* value) { return *value; }

struct rw_lock {};
#define RW_LOCK_INITIALIZER(name) {}
struct ReadLocker { ReadLocker(rw_lock&) {} void Unlock() {} };
struct WriteLocker { WriteLocker(rw_lock&) {} };

struct module_info {};
struct net_socket { int id; bool closed; bool freed; };
struct sockaddr {};
struct io_context {};
struct file_descriptor { void* cookie; };
struct FileDescriptorPutter { FileDescriptorPutter(file_descriptor*) {} };

struct net_stack_interface_module_info {
    status_t (*accept)(net_socket* socket, sockaddr* address,
        socklen_t* _addressLength, net_socket** _acceptedSocket);
    status_t (*close)(net_socket* socket);
    status_t (*free)(net_socket* socket);
    status_t (*socketpair)(int family, int type, int protocol,
        net_socket* sockets[2]);
};

static file_descriptor sListenerDescriptor;
#define FD_SOCKET(descriptor) ((net_socket*)descriptor->cookie)
#define GET_SOCKET_FD_OR_RETURN(fd, kernel, descriptor) \
    do { descriptor = &sListenerDescriptor; } while (false)
#define RETURN_AND_SET_ERRNO(error) return (error)

static status_t get_module(const char* name, module_info** _module);
static status_t put_module(const char* name);
static int create_socket_fd(net_socket* socket, int flags, bool kernel);
static status_t close_fd_index(io_context* context, int fd);
static io_context* get_current_io_context(bool kernel);
'''

harness = r'''
// #pragma mark - test harness

static int sModuleLoads, sModuleUnloads;

#define MAX_SOCKETS 16
static net_socket sSockets[MAX_SOCKETS];
static int sSocketCount, sClosedCount, sFreedCount;

static net_socket* make_socket()
{
    assert(sSocketCount < MAX_SOCKETS);
    net_socket* socket = &sSockets[sSocketCount];
    socket->id = sSocketCount++;
    socket->closed = socket->freed = false;
    return socket;
}
static status_t stack_close(net_socket* socket)
{
    assert(!socket->closed);
    socket->closed = true;
    sClosedCount++;
    return B_OK;
}
static status_t stack_free(net_socket* socket)
{
    assert(socket->closed && !socket->freed);
    socket->freed = true;
    sFreedCount++;
    return B_OK;
}
static status_t stack_accept(net_socket*, sockaddr*, socklen_t*,
    net_socket** _acceptedSocket)
{
    *_acceptedSocket = make_socket();
    return B_OK;
}
static status_t stack_socketpair(int, int, int, net_socket* sockets[2])
{
    sockets[0] = make_socket();
    sockets[1] = make_socket();
    return B_OK;
}

static net_stack_interface_module_info sModule
    = { stack_accept, stack_close, stack_free, stack_socketpair };

static status_t get_module(const char*, module_info** _module)
{
    sModuleLoads++;
    *_module = (module_info*)&sModule;
    return B_OK;
}
static status_t put_module(const char*)
{
    sModuleUnloads++;
    // The stack may never be torn down while any socket it allocated is
    // still live: a reload would hand the new instance sockets of the
    // torn-down one, and the reload itself may fail.
    for (int i = 0; i < sSocketCount; i++)
        assert(sSockets[i].freed);
    return B_OK;
}

#define MAX_FDS 8
static net_socket* sFDTable[MAX_FDS];
static int sNextFD;
static int sCreateCalls, sFailCreateAt = -1;
static bool sConcurrentCloser;
static io_context sIOContext;

static io_context* get_current_io_context(bool) { return &sIOContext; }

static int publish_fd(net_socket* socket)
{
    assert(sNextFD < MAX_FDS);
    int fd = sNextFD++;
    sFDTable[fd] = socket;
    return fd;
}
static int live_fds()
{
    int count = 0;
    for (int i = 0; i < sNextFD; i++) {
        if (sFDTable[i] != NULL)
            count++;
    }
    return count;
}
static status_t close_fd_index(io_context*, int fd)
{
    if (fd < 0 || fd >= sNextFD || sFDTable[fd] == NULL)
        return B_FILE_ERROR;
    net_socket* socket = sFDTable[fd];
    sFDTable[fd] = NULL;
    // mirrors socket_close() + socket_free()
    sStackInterface->close(socket);
    sStackInterface->free(socket);
    put_stack_interface_module();
    return B_OK;
}
static int create_socket_fd(net_socket* socket, int, bool)
{
    if (sCreateCalls++ == sFailCreateAt)
        return B_NO_MEMORY;
    int fd = publish_fd(socket);
    if (sConcurrentCloser) {
        // Another thread may close() the FD the moment it is published (fd
        // numbers are deterministic lowest-free). The FD must already own
        // its stack interface reference, so this close may neither drive
        // the consumer count negative nor (under KDEBUG) unload the net
        // stack while other FDs are still live.
        close_fd_index(&sIOContext, fd);
        if (live_fds() > 0)
            assert(sStackInterface != NULL);
    }
    return fd;
}

static void reset()
{
    assert(sStackInterfaceConsumers == 0);
    sSocketCount = sClosedCount = sFreedCount = 0;
    memset(sFDTable, 0, sizeof(sFDTable));
    sNextFD = 0;
    sCreateCalls = 0;
    sFailCreateAt = -1;
    sConcurrentCloser = false;
    sModuleLoads = sModuleUnloads = 0;
}
static int make_listener()
{
    // a listening socket FD, holding its own stack interface reference
    net_stack_interface_module_info* module = get_stack_interface_module();
    assert(module != NULL);
    (void)module;
    net_socket* listener = make_socket();
    sListenerDescriptor.cookie = listener;
    return publish_fd(listener);
}
'''

main = r'''
int main()
{
    // accept(): a concurrent close() of the just-published FD must not
    // unload the net stack under the listening socket
    reset();
    int listenerFD = make_listener();
    sConcurrentCloser = true;
    int fd = common_accept(listenerFD, NULL, NULL, 0, false);
    assert(fd >= 0 && sFDTable[fd] == NULL);
    assert(sStackInterfaceConsumers == 1 && sStackInterface != NULL);
    sConcurrentCloser = false;
    assert(close_fd_index(&sIOContext, listenerFD) == B_OK);
    assert(sStackInterfaceConsumers == 0 && sStackInterface == NULL);
    assert(sClosedCount == 2 && sFreedCount == 2);
    assert(sModuleLoads == 1 && sModuleUnloads == 1);

    // accept(): FD creation failure disposes of the accepted socket and
    // releases the reference taken for it
    reset();
    listenerFD = make_listener();
    sFailCreateAt = sCreateCalls;
    fd = common_accept(listenerFD, NULL, NULL, 0, false);
    assert(fd < 0);
    assert(sStackInterfaceConsumers == 1 && sStackInterface != NULL);
    assert(sClosedCount == 1 && sFreedCount == 1);
    assert(close_fd_index(&sIOContext, listenerFD) == B_OK);
    assert(sStackInterfaceConsumers == 0);

    // accept(): plain success keeps one reference per FD
    reset();
    listenerFD = make_listener();
    fd = common_accept(listenerFD, NULL, NULL, 0, false);
    assert(fd >= 0 && sStackInterfaceConsumers == 2);
    assert(close_fd_index(&sIOContext, fd) == B_OK);
    assert(close_fd_index(&sIOContext, listenerFD) == B_OK);
    assert(sStackInterfaceConsumers == 0 && sClosedCount == 2);

    // socketpair(): concurrent close() of each FD at publication must not
    // drive the consumer count negative (checked in atomic_add()) nor ever
    // unload the stack under the pair's other socket (checked in
    // put_module()): both references are taken before anything is published
    reset();
    int fds[2];
    sConcurrentCloser = true;
    assert(common_socketpair(0, 0, 0, fds, false) == B_OK);
    assert(live_fds() == 0 && sStackInterfaceConsumers == 0);
    assert(sClosedCount == 2 && sFreedCount == 2);
    assert(sModuleLoads == 1 && sModuleUnloads == 1);

    // socketpair(): failure publishing the first FD disposes of both sockets
    reset();
    sFailCreateAt = 0;
    assert(common_socketpair(0, 0, 0, fds, false) < 0);
    assert(live_fds() == 0 && sStackInterfaceConsumers == 0);
    assert(sClosedCount == 2 && sFreedCount == 2);
    assert(sModuleLoads == sModuleUnloads);

    // socketpair(): failure publishing the second FD also closes the
    // already published first FD (and with it sockets[0])
    reset();
    sFailCreateAt = 1;
    assert(common_socketpair(0, 0, 0, fds, false) < 0);
    assert(live_fds() == 0 && sStackInterfaceConsumers == 0);
    assert(sClosedCount == 2 && sFreedCount == 2);
    assert(sModuleLoads == sModuleUnloads);

    // socketpair(): plain success keeps one reference per FD
    reset();
    assert(common_socketpair(0, 0, 0, fds, false) == B_OK);
    assert(live_fds() == 2 && sStackInterfaceConsumers == 2);
    assert(close_fd_index(&sIOContext, fds[0]) == B_OK);
    assert(sStackInterfaceConsumers == 1 && sStackInterface != NULL);
    assert(close_fd_index(&sIOContext, fds[1]) == B_OK);
    assert(sStackInterfaceConsumers == 0 && sClosedCount == 2);

    puts("PASS: every published socket FD owns its stack interface"
        " reference and failure paths dispose of all sockets and FDs");
    return 0;
}
'''

code = (prelude + '\n' + interface + '\n' + harness + '\n' + accept + '\n\n'
        + socketpair + '\n' + main)

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'test.cpp'
    path.write_text(code)
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                    '-fsanitize=address,undefined', str(path), '-o',
                    str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
