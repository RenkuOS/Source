/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FBSD_COMPAT_MACHINE_ATOMIC_H_
#define _FBSD_COMPAT_MACHINE_ATOMIC_H_


#include <KernelExport.h>


#define	atomic_load_32(ptr)			atomic_get((int32*)ptr)
#define atomic_set_32(ptr, val)		(void)atomic_set((int32*)ptr, val)
#define atomic_fetchadd_32(ptr, val) atomic_add((int32*)ptr, val)
#define atomic_add_32(ptr, val)		(void)atomic_add((int32*)ptr, val)
#define atomic_subtract_32(ptr, val) (void)atomic_add((int32*)ptr, -val)
#define atomic_readandclear_32(ptr) atomic_set((int32*)ptr, 0)
#define atomic_cmpset_32(ptr, old, new) \
	(atomic_test_and_set((int32*)ptr, new, old) == old)

#define atomic_load_acq_32		atomic_load_32
#define atomic_set_acq_32		atomic_set_32

#define	atomic_load_64(ptr)		atomic_get64((int64*)ptr)
#define atomic_cmpset_64(ptr, old, new) \
	(atomic_test_and_set64((int64*)ptr, new, old) == old)


#define atomic_add_int			atomic_add_32
#define atomic_subtract_int		atomic_subtract_32
#define atomic_set_int			atomic_set_32
#define atomic_load_int			atomic_load_32
#define atomic_cmpset_int		atomic_cmpset_32

#define atomic_store_rel_int	atomic_set_32
#define atomic_cmpset_acq_int	atomic_cmpset_32
#define atomic_readandclear_int	atomic_readandclear_32

#define	atomic_load_long		atomic_load_64
#define	atomic_load_acq_long	atomic_load_64
#define atomic_cmpset_long		atomic_cmpset_64
// Unlike atomic_set_32 above, these follow FreeBSD: set and clear bits.
#define atomic_set_long(ptr, bits)	(void)atomic_or64((int64*)ptr, bits)
#define atomic_clear_long(ptr, bits) (void)atomic_and64((int64*)ptr, ~(bits))


#define mb()    memory_full_barrier()
#define wmb()   memory_write_barrier()
#define rmb()   memory_read_barrier()


#endif	/* _FBSD_COMPAT_MACHINE_ATOMIC_H_ */
