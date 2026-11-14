/*
 * Copyright 2019, Ryan Leavengood.
 * Copyright 2009, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002, Marcus Overhagen. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


//! A cache for BBuffers to be received by BBufferConsumer::BufferReceived().


#include "BufferCache.h"

#include <Buffer.h>

#include "MediaDebug.h"
#include "MediaMisc.h"
#include "SharedBufferList.h"


namespace BPrivate {


BufferCache::BufferCache()
{
}


BufferCache::~BufferCache()
{
	BufferMap::Iterator iterator = fMap.GetIterator();
	while (iterator.HasNext()) {
		BufferMap::Entry entry = iterator.Next();
		delete entry.value.buffer;
	}
}


BBuffer*
BufferCache::GetBuffer(media_buffer_id id, const media_header& header)
{
	if (id <= 0)
		return NULL;

	buffer_cache_entry* existing;
	if (fMap.Get(id, existing)) {
		existing->port = header.source_port;
		existing->source = header.source;
		existing->destination = header.destination;
		existing->buffer->fFlags |= BUFFER_TO_RECLAIM;
		return existing->buffer;
	}

	buffer_clone_info info;
	info.buffer = id;
	BBuffer* buffer = new(std::nothrow) BBuffer(info);
	if (buffer == NULL || buffer->ID() <= 0
			|| buffer->Data() == NULL) {
		delete buffer;
		return NULL;
	}

	if (buffer->ID() != id)
		debugger("BufferCache::GetBuffer: IDs mismatch");

	buffer_cache_entry entry;
	entry.buffer = buffer;
	entry.port = header.source_port;
	entry.source = header.source;
	entry.destination = header.destination;
	status_t error = fMap.Put(id, entry);
	if (error != B_OK) {
		delete buffer;
		return NULL;
	}

	buffer->fFlags |= BUFFER_TO_RECLAIM;
	return buffer;
}


void
BufferCache::FlushCacheForPort(port_id port)
{
	_Flush(port, false, 0, 0);
}


/*!	Flushes only the buffers that last came in on the connection from
	\a source of the producer at \a port to our input \a destination, for
	when the producer has other connections to us that still use theirs.
*/
void
BufferCache::FlushCacheForConnection(port_id port, int32 source,
	int32 destination)
{
	_Flush(port, true, source, destination);
}


void
BufferCache::_Flush(port_id port, bool connectionOnly, int32 source,
	int32 destination)
{
	BufferMap::Iterator iterator = fMap.GetIterator();
	while (iterator.HasNext()) {
		BufferMap::Entry entry = iterator.Next();
		if (entry.value.port == port
			&& (!connectionOnly || (entry.value.source == source
				&& entry.value.destination == destination))) {
			BBuffer* buffer = entry.value.buffer;
			bool isReclaimed = (buffer->fFlags & BUFFER_TO_RECLAIM) == 0;
			if (isReclaimed && buffer->fBufferList != NULL)
				isReclaimed = buffer->fBufferList->RemoveBuffer(buffer) == B_OK;

			if (isReclaimed)
				delete buffer;
			else {
				// mark the buffer for deletion
				buffer->fFlags |= BUFFER_MARKED_FOR_DELETION;
			}
			// Then remove it from the map
			fMap.Remove(iterator);
		}
	}
}


}	// namespace BPrivate
