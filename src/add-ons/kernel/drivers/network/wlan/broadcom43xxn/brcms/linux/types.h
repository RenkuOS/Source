/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * What the imported brcmsmac sources expect of their surroundings. They are
 * taken unchanged from Linux, so the names they use are provided here rather
 * than edited out of forty thousand lines.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BRCMS_LINUX_TYPES_H
#define BRCMS_LINUX_TYPES_H


#include <KernelExport.h>
#include <SupportDefs.h>

#include <stdlib.h>
#include <string.h>


typedef uint8 u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;
typedef int8 s8;
typedef int16 s16;
typedef int32 s32;
typedef int64 s64;

// What the card's DMA engine is given, which here is a plain physical
// address, and Linux's allocation flags, which nothing acts on.
typedef phys_addr_t	dma_addr_t;
typedef unsigned int	gfp_t;

typedef uint16 __le16;
typedef uint32 __le32;
typedef uint16 __be16;
typedef uint32 __be32;

#define __iomem
#define __must_check
#define __packed	__attribute__((packed))

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))
#define ABS(x)			((x) < 0 ? -(x) : (x))
#define min_t(type, a, b)	((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define max_t(type, a, b)	((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define BIT(n)			(1UL << (n))

#ifndef PAGE_SIZE
#	define PAGE_SIZE	B_PAGE_SIZE
#endif

/* Register polling also runs with interrupts disabled and a spinlock
 * held. Linux udelay must not sleep or yield in that context. */
#define udelay(x)		spin(x)
#define mdelay(x)		snooze((x) * 1000LL)
#define usleep_range(a, b)	snooze(a)

#define WARN_ON(x)		(x)
#define WARN(x, ...)	(x)
#define BUG_ON(x)		do {} while (0)

#define cpu_to_le16(x)	((__le16)(x))
#define le16_to_cpu(x)	((uint16)(x))
#define cpu_to_le32(x)	((__le32)(x))
#define cpu_to_be32(x)	((__be32)__builtin_bswap32(x))
#define cpu_to_be16(x)	((__be16)__builtin_bswap16(x))
#define le32_to_cpu(x)	((uint32)(x))

#define CORDIC_FLOAT(x)	(x)


typedef uint8 __u8;
typedef uint16 __u16;
typedef uint32 __u32;
typedef int8 __s8;
typedef int16 __s16;
typedef int32 __s32;

// The imported sources declare these but the phy layer never contends for
// anything: it runs with the MAC suspended, under the caller's own lock.
typedef struct { int unused; } spinlock_t;
struct mutex { int unused; };

// Counters and a sleep queue the driver structure carries but the phy layer
// never touches.
typedef struct { int counter; } atomic_t;
typedef struct { int unused; } wait_queue_head_t;

#ifndef ENOTSUPP
#define ENOTSUPP	ENOTSUP
#endif

#ifndef ENOMEDIUM
#define ENOMEDIUM	ENODEV
#endif


#define __aligned(x)	__attribute__((aligned(x)))
#define __always_inline	inline
#define __maybe_unused	__attribute__((unused))

#define container_of(pointer, type, member) \
	((type*)((char*)(pointer) - offsetof(type, member)))

#define GFP_ATOMIC	0
#define GFP_KERNEL	0

#ifndef EBADE
#define EBADE		B_ERROR
#endif

typedef int pm_message_t;
typedef int pci_power_t;

// The driver model and buffer types the imported headers name but the phy
// layer never touches. They exist so the structures around it parse.
struct device { int unused; };
struct device_driver { int unused; };
struct module { int unused; };
struct list_head { struct list_head* next; struct list_head* prev; };
// The phy uses the cordic rotation to work out tone phases.



#define ETH_ALEN	6

struct ieee80211_rts { uint16 frame_control; uint16 duration;
	uint8 ra[6]; uint8 ta[6]; } __packed;
struct ieee80211_cts { uint16 frame_control; uint16 duration;
	uint8 ra[6]; } __packed;



#define __printf(a, b)	__attribute__((format(printf, a, b)))

#define kmalloc_objs(type, count, flags) \
	((type*)malloc(sizeof(type) * (count)))
#define kzalloc(size, flags)	calloc(1, size)
#define kfree(pointer)			free(pointer)



#define DIV_ROUND_CLOSEST(x, divisor) \
	(((x) + ((divisor) / 2)) / (divisor))
#define IS_ALIGNED(x, a)	(((x) & ((typeof(x))(a) - 1)) == 0)
#define strscpy(to, from, size)	strlcpy(to, from, size)
#define printk_once(...)	do {} while (0)

#define roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))

#define kmalloc(size, flags)			malloc(size)
#define kmalloc_array(count, size, flags)	malloc((count) * (size))
#define kzalloc_obj(object, flags)		calloc(1, sizeof(object))
#define kvmemdup(data, size, flags)		memcpy(malloc(size), data, size)
#define kvfree(pointer)					free(pointer)

#define pr_debug(...)	do {} while (0)
#define pr_err(...)		do {} while (0)
#define pr_info(...)	do {} while (0)

/*!	The position of the highest bit set, counting from one. */
static inline int
fls(unsigned int value)
{
	int position = 0;
	while (value != 0) {
		position++;
		value >>= 1;
	}
	return position;
}


/*!	How many bits are set in a byte. */
static inline unsigned int
hweight8(uint8 value)
{
	unsigned int count = 0;
	while (value != 0) {
		count += value & 1;
		value >>= 1;
	}
	return count;
}


/*!	The whole part of the square root, by bit at a time. */
static inline unsigned long
int_sqrt(unsigned long value)
{
	unsigned long result = 0;
	unsigned long bit = 1UL << (sizeof(value) * 8 - 2);

	while (bit > value)
		bit >>= 2;

	while (bit != 0) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else
			result >>= 1;
		bit >>= 2;
	}

	return result;
}


#endif	// BRCMS_LINUX_TYPES_H
