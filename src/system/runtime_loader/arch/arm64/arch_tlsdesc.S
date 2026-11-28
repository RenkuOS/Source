/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include <asm_defs.h>


/*	TLS descriptor resolvers, set up for R_AARCH64_TLSDESC relocations in
	arch_relocate.cpp. Compiled code calls them with x0 pointing to the
	descriptor {resolver, argument} and adds the returned x0 to TPIDR_EL0.
	The AArch64 TLSDESC ABI requires them to preserve every other register,
	except the condition flags.
*/

.text


/*	Undefined weak symbol: the argument is the variable's address (the
	addend), as for any undefined weak symbol.
*/
.hidden __arch_tlsdesc_undefined_weak
FUNCTION(__arch_tlsdesc_undefined_weak):
	str		x1, [sp, #-16]!
	ldr		x0, [x0, #8]
	mrs		x1, tpidr_el0
	sub		x0, x0, x1
	ldr		x1, [sp], #16
	ret
FUNCTION_END(__arch_tlsdesc_undefined_weak)


/*	The argument is passed to __arch_tlsdesc_get_address(), which may
	allocate the thread's TLS block. The C ABI lets that change x1-x18, x29,
	x30 and all of v0-v31 (only the low halves of v8-v15 are callee-saved),
	so they are saved here.
	Only q0-q31 are saved, not the SVE z and p registers: this must change
	if userland SVE is ever enabled.
*/
.hidden __arch_tlsdesc_dynamic
FUNCTION(__arch_tlsdesc_dynamic):
	stp		x29, x30, [sp, #-(10 * 16)]!
	mov		x29, sp
	stp		x1, x2, [sp, #(1 * 16)]
	stp		x3, x4, [sp, #(2 * 16)]
	stp		x5, x6, [sp, #(3 * 16)]
	stp		x7, x8, [sp, #(4 * 16)]
	stp		x9, x10, [sp, #(5 * 16)]
	stp		x11, x12, [sp, #(6 * 16)]
	stp		x13, x14, [sp, #(7 * 16)]
	stp		x15, x16, [sp, #(8 * 16)]
	stp		x17, x18, [sp, #(9 * 16)]
	sub		sp, sp, #(16 * 32)
	stp		q0, q1, [sp, #(0 * 32)]
	stp		q2, q3, [sp, #(1 * 32)]
	stp		q4, q5, [sp, #(2 * 32)]
	stp		q6, q7, [sp, #(3 * 32)]
	stp		q8, q9, [sp, #(4 * 32)]
	stp		q10, q11, [sp, #(5 * 32)]
	stp		q12, q13, [sp, #(6 * 32)]
	stp		q14, q15, [sp, #(7 * 32)]
	stp		q16, q17, [sp, #(8 * 32)]
	stp		q18, q19, [sp, #(9 * 32)]
	stp		q20, q21, [sp, #(10 * 32)]
	stp		q22, q23, [sp, #(11 * 32)]
	stp		q24, q25, [sp, #(12 * 32)]
	stp		q26, q27, [sp, #(13 * 32)]
	stp		q28, q29, [sp, #(14 * 32)]
	stp		q30, q31, [sp, #(15 * 32)]

	ldr		x0, [x0, #8]
	bl		__arch_tlsdesc_get_address
	mrs		x1, tpidr_el0
	sub		x0, x0, x1

	ldp		q0, q1, [sp, #(0 * 32)]
	ldp		q2, q3, [sp, #(1 * 32)]
	ldp		q4, q5, [sp, #(2 * 32)]
	ldp		q6, q7, [sp, #(3 * 32)]
	ldp		q8, q9, [sp, #(4 * 32)]
	ldp		q10, q11, [sp, #(5 * 32)]
	ldp		q12, q13, [sp, #(6 * 32)]
	ldp		q14, q15, [sp, #(7 * 32)]
	ldp		q16, q17, [sp, #(8 * 32)]
	ldp		q18, q19, [sp, #(9 * 32)]
	ldp		q20, q21, [sp, #(10 * 32)]
	ldp		q22, q23, [sp, #(11 * 32)]
	ldp		q24, q25, [sp, #(12 * 32)]
	ldp		q26, q27, [sp, #(13 * 32)]
	ldp		q28, q29, [sp, #(14 * 32)]
	ldp		q30, q31, [sp, #(15 * 32)]
	add		sp, sp, #(16 * 32)
	ldp		x1, x2, [sp, #(1 * 16)]
	ldp		x3, x4, [sp, #(2 * 16)]
	ldp		x5, x6, [sp, #(3 * 16)]
	ldp		x7, x8, [sp, #(4 * 16)]
	ldp		x9, x10, [sp, #(5 * 16)]
	ldp		x11, x12, [sp, #(6 * 16)]
	ldp		x13, x14, [sp, #(7 * 16)]
	ldp		x15, x16, [sp, #(8 * 16)]
	ldp		x17, x18, [sp, #(9 * 16)]
	ldp		x29, x30, [sp], #(10 * 16)
	ret
FUNCTION_END(__arch_tlsdesc_dynamic)
