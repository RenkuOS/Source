/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "arch_elf.h"
#include "elf_tls.h"
#include "runtime_loader_private.h"

#include <runtime_loader.h>


//#define TRACE_RLD
#ifdef TRACE_RLD
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


// TLS descriptor resolvers, in arch_tlsdesc.S
extern "C" void __arch_tlsdesc_dynamic() __attribute__((visibility("hidden")));
extern "C" void __arch_tlsdesc_undefined_weak()
	__attribute__((visibility("hidden")));


/*!	Called by __arch_tlsdesc_dynamic() with the descriptor's argument: the
	module (DSO) in its high and the offset into the module's TLS block in its
	low 32 bits.
	Packing both into the descriptor, which is in the image's GOT, means there
	is nothing to allocate or free with the image.
*/
extern "C" __attribute__((visibility("hidden"))) void*
__arch_tlsdesc_get_address(uint64 argument)
{
	return get_tls_address(argument >> 32, (uint32)argument);
}


static status_t
relocate_rela(image_t* rootImage, image_t* image, Elf64_Rela* rel,
	size_t relLength, SymbolLookupCache* cache)
{
	for (size_t i = 0; i < relLength / sizeof(Elf64_Rela); i++) {
		int type = ELF64_R_TYPE(rel[i].r_info);
		int symIndex = ELF64_R_SYM(rel[i].r_info);
		Elf64_Addr symAddr = 0;
		image_t* symbolImage = NULL;

		// Resolve the symbol, if any.
		if (symIndex != 0) {
			Elf64_Sym* sym = SYMBOL(image, symIndex);

			status_t status = resolve_symbol(rootImage, image, sym, cache,
				&symAddr, &symbolImage);
			if (status != B_OK) {
				TRACE(("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status));
				printf("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status);
				return status;
			}
		}

		// Address of the relocation.
		Elf64_Addr relocAddr = image->regions[0].delta + rel[i].r_offset;

		// Calculate the relocation value.
		Elf64_Addr relocValue;
		switch (type) {
			case R_AARCH64_NONE:
				continue;
			case R_AARCH64_ABS64:
			case R_AARCH64_GLOB_DAT:
			case R_AARCH64_JUMP_SLOT:
				relocValue = symAddr + rel[i].r_addend;
				break;
			case R_AARCH64_RELATIVE:
				relocValue = image->regions[0].delta + rel[i].r_addend;
				break;
			case R_AARCH64_TLS_DTPMOD64:
				relocValue = symbolImage == NULL
							? image->dso_tls_id : symbolImage->dso_tls_id;
				break;
			case R_AARCH64_TLS_DTPREL64:
				relocValue = symAddr;
				break;
			case R_AARCH64_TLSDESC:
			{
				// There is no static TLS, so the variable's address is always
				// looked up in the thread's dynamic thread vector.
				Elf64_Addr* descriptor = (Elf64_Addr*)relocAddr;
				if (symIndex != 0 && symbolImage == NULL) {
					descriptor[0] = (Elf64_Addr)__arch_tlsdesc_undefined_weak;
					descriptor[1] = rel[i].r_addend;
					continue;
				}

				unsigned dso = symbolImage == NULL
					? image->dso_tls_id : symbolImage->dso_tls_id;
				Elf64_Addr offset = symAddr + rel[i].r_addend;
				if (dso == unsigned(-1) || offset > UINT32_MAX) {
					TRACE(("bad TLS descriptor: dso %u, offset %#lx\n", dso,
						offset));
					return B_BAD_DATA;
				}
				descriptor[0] = (Elf64_Addr)__arch_tlsdesc_dynamic;
				descriptor[1] = (Elf64_Addr)dso << 32 | offset;
				continue;
			}
			default:
				TRACE(("unhandled relocation type %d\n", type));
				return B_BAD_DATA;
		}

		*(Elf64_Addr *)relocAddr = relocValue;
	}

	return B_OK;
}


status_t
arch_relocate_image(image_t *rootImage, image_t *image,
	SymbolLookupCache* cache)
{
	status_t status;

	// No REL on arm64.

	// Perform RELA relocations.
	if (image->rela) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache);
		if (status != B_OK)
			return status;
	}

	// PLT relocations (they are RELA on arm64).
	if (image->pltrel) {
		status = relocate_rela(rootImage, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, cache);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}
