/*
 * Copyright 2026, Andrea Bernardi
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


#include <elf.h>
#include <image.h>
#include <stdint.h>


#ifndef PT_ARM_EXIDX
#	define PT_ARM_EXIDX 0x70000001
#endif


/*!	Called by the ARM EHABI unwinder of libgcc to find the exception index
	table (.ARM.exidx) covering \a pc. Without it libgcc only knows its own
	table, and every C++ exception ends in std::terminate().
	Sets \a _count to the number of 8 byte entries.
*/
extern "C" uintptr_t
__gnu_Unwind_Find_exidx(uintptr_t pc, int* _count)
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		uintptr_t text = (uintptr_t)info.text;
		if (pc < text || pc - text >= (uintptr_t)info.text_size)
			continue;

		// the first segment starts with the ELF header and the program
		// headers
		const Elf32_Ehdr* header = (const Elf32_Ehdr*)text;
		if (header->e_ident[EI_MAG0] != ELFMAG0
			|| header->e_ident[EI_MAG1] != ELFMAG1
			|| header->e_ident[EI_MAG2] != ELFMAG2
			|| header->e_ident[EI_MAG3] != ELFMAG3) {
			return 0;
		}

		const Elf32_Phdr* programHeaders
			= (const Elf32_Phdr*)(text + header->e_phoff);
		uintptr_t delta = 0;
		bool foundLoad = false;
		for (int i = 0; i < header->e_phnum; i++) {
			if (programHeaders[i].p_type == PT_LOAD && !foundLoad) {
				// the load address of the first segment
				delta = text - (programHeaders[i].p_vaddr
					& ~(uintptr_t)(B_PAGE_SIZE - 1));
				foundLoad = true;
			}
		}
		for (int i = 0; i < header->e_phnum; i++) {
			if (programHeaders[i].p_type == PT_ARM_EXIDX) {
				*_count = programHeaders[i].p_memsz / 8;
				return programHeaders[i].p_vaddr + delta;
			}
		}
		return 0;
	}

	return 0;
}
