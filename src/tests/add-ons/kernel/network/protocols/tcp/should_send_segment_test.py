#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run TCPEndpoint::_ShouldSendSegment() from TCPEndpoint.cpp: with
TCP_NODELAY set, a sub-MSS piece in the middle of the send queue (the
window allows less than one full segment while more data is queued) must
wait for the window to open instead of going out as a runt segment.
Sender-side silly window avoidance and the Nagle algorithm are separate
mechanisms (RFC 1122 4.2.3.4); TCP_NODELAY only disables the latter.
The short tail of the queued data still goes out at once, with or
without TCP_NODELAY, so interactive latency is unchanged."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[7])
source = (root / 'src/add-ons/kernel/network/protocols/tcp/'
        'TCPEndpoint.cpp').read_text()
start = source.index('inline bool\nTCPEndpoint::_ShouldSendSegment(')
function = source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint32_t uint32;
typedef int32_t int32;

#define TCP_NODELAY 0x01

#define TCP_FLAG_SYNCHRONIZE 0x02
#define TCP_FLAG_RESET 0x04
#define TCP_FLAG_FINISH 0x01

enum tcp_state {
	CLOSED,
	LISTEN,
	SYNCHRONIZE_SENT,
	SYNCHRONIZE_RECEIVED,
	ESTABLISHED,
};

class tcp_sequence {
public:
	tcp_sequence() : fNumber(0) {}
	tcp_sequence(uint32 number) : fNumber(number) {}
	uint32 Number() const { return fNumber; }
	bool operator==(tcp_sequence other) const
		{ return fNumber == other.fNumber; }
	bool operator>(tcp_sequence other) const
		{ return (int32)(fNumber - other.fNumber) > 0; }
	tcp_sequence operator+(uint32 value) const
		{ return tcp_sequence(fNumber + value); }
	tcp_sequence operator+(int value) const
		{ return tcp_sequence(fNumber + (uint32)value); }
	tcp_sequence operator-(tcp_sequence other) const
		{ return tcp_sequence(fNumber - other.fNumber); }
private:
	uint32 fNumber;
};

struct tcp_segment_header {
	uint32 advertised_window;
	uint8_t flags;
	uint32 AdvertisedWindow(uint8_t windowShift) const
		{ return advertised_window << windowShift; }
};

struct net_socket {
	struct {
		uint32 buffer_size;
	} receive;
};

struct SendQueue {
	tcp_sequence last;
	tcp_sequence LastSequence() const { return last; }
};

class TCPEndpoint {
public:
	bool _ShouldSendSegment(tcp_segment_header& segment, uint32 length,
		uint32 segmentMaxSize, uint32 flightSize);

	net_socket* socket;
	tcp_state fState;
	uint32 fSendMaxSegments;
	uint32 fOptions;
	tcp_sequence fSendNext;
	tcp_sequence fSendUrgentOffset;
	SendQueue fSendQueue;
	uint32 fSendMaxWindow;
	uint8_t fReceiveWindowShift;
	tcp_sequence fReceiveNext;
	tcp_sequence fReceiveMaxAdvertised;
	uint32 fReceiveMaxSegmentSize;
};
'''

main = r'''
static TCPEndpoint endpoint;
static net_socket sock;
static tcp_segment_header segment;

// Start from fSendNext with `queued` bytes in the send queue; each check
// passes the length of the piece the window allows to go out now.
static void
reset(uint32 queued)
{
	endpoint = TCPEndpoint();
	segment = tcp_segment_header();
	sock.receive.buffer_size = 65536;
	endpoint.socket = &sock;
	endpoint.fState = ESTABLISHED;
	endpoint.fSendMaxSegments = 10;
	endpoint.fSendNext = tcp_sequence(1000);
	endpoint.fSendUrgentOffset = tcp_sequence(0);
	endpoint.fSendQueue.last = tcp_sequence(1000 + queued);
	endpoint.fSendMaxWindow = 65535;
	endpoint.fReceiveMaxSegmentSize = 1460;
}

int
main()
{
	const uint32 kMaxSegmentSize = 1460;

	// A 100 byte piece with 1560 bytes queued is not at the end of the
	// queue, not a full segment and not half the maximum window: it must
	// wait, TCP_NODELAY or not. This is the case that used to go out as
	// a runt segment when TCP_NODELAY was set.
	reset(1560);
	endpoint.fOptions = TCP_NODELAY;
	assert(!endpoint._ShouldSendSegment(segment, 100, kMaxSegmentSize, 0));

	reset(1560);
	endpoint.fOptions = 0;
	assert(!endpoint._ShouldSendSegment(segment, 100, kMaxSegmentSize, 0));

	// The short tail of the queued data still goes out at once, with and
	// without TCP_NODELAY.
	reset(100);
	endpoint.fOptions = TCP_NODELAY;
	assert(endpoint._ShouldSendSegment(segment, 100, kMaxSegmentSize, 0));

	reset(100);
	endpoint.fOptions = 0;
	assert(endpoint._ShouldSendSegment(segment, 100, kMaxSegmentSize, 0));

	// A full segment always goes out.
	reset(4000);
	endpoint.fOptions = TCP_NODELAY;
	assert(endpoint._ShouldSendSegment(segment, kMaxSegmentSize,
		kMaxSegmentSize, 0));

	// So does a piece of at least half the maximum send window.
	reset(4000);
	endpoint.fOptions = 0;
	endpoint.fSendMaxWindow = 1200;
	assert(endpoint._ShouldSendSegment(segment, 600, kMaxSegmentSize, 0));

	// No congestion window left: nothing goes out.
	reset(4000);
	endpoint.fSendMaxSegments = 0;
	assert(!endpoint._ShouldSendSegment(segment, kMaxSegmentSize,
		kMaxSegmentSize, 0));

	printf("all _ShouldSendSegment checks passed\n");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'test.cpp'
    test.write_text(fixture + function + main)
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall',
        '-Werror', '-fsanitize=address,undefined', '-o', str(binary),
        str(test)], check=True)
    subprocess.run([str(binary)], check=True)
