/*
 * Copyright 2019, Ryan Leavengood.
 * Copyright 2009, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002, Marcus Overhagen. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUFFER_CACHE_H_
#define _BUFFER_CACHE_H_


#include <HashMap.h>
#include <MediaDefs.h>


class BBuffer;


namespace BPrivate {


struct buffer_cache_entry {
	BBuffer*	buffer;
	port_id		port;
	int32		source;
	int32		destination;
		// the connection the buffer last came in on
};


class BufferCache {
public:
								BufferCache();
								~BufferCache();

			BBuffer*			GetBuffer(media_buffer_id id,
									const media_header& header);

			void				FlushCacheForPort(port_id port);
			void				FlushCacheForConnection(port_id port,
									int32 source, int32 destination);

private:
			void				_Flush(port_id port, bool connectionOnly,
									int32 source, int32 destination);

private:
	typedef HashMap<HashKey32<media_buffer_id>, buffer_cache_entry> BufferMap;

			BufferMap			fMap;
};


}	// namespace BPrivate


#endif	// _BUFFER_CACHE_H_
