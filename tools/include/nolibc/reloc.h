/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * Self relocation support for NOLIBC
 * Copyright (C) 2026 Daniel Palmer<daniel@thingy.jp>
 *
 * This allows a PIE compiled nolibc binary relocate itself
 * instead of relying on a dynamic linker. So called "static PIE".
 *
 * With some care binaries produced with this relocation code
 * can run via the FDPIC ELF loader on nommu systems.
 *
 * I am not expert in all of the different options for GCC but
 * this works for me:
 * gcc -flto -nostdlib -fpie -Os -include <path to nolibc.h> \
 * -Wl,--no-dynamic-linker -pie -o helloworld helloworld.c
 */

#ifndef _NOLIBC_RELOC_H
#define _NOLIBC_RELOC_H

#ifdef NOLIBC_ARCH_HAS_RELOC
#include "elf.h"
#include <linux/auxvec.h>

#ifdef NOLIBC_ARCH_ELF32
#define elf_ehdr Elf32_Ehdr
#define elf_phdr Elf32_Phdr
#define elf_dyn  Elf32_Dyn
#define elf_rela Elf32_Rela
#else
#define elf_ehdr Elf64_Ehdr
#define elf_phdr Elf64_Phdr
#define elf_dyn  Elf64_Dyn
#define elf_rela Elf64_Rela
#endif

/*
 * Your arch needs to provide this to actually handle doing each of the
 * relocations.
 */
static int _process_reloc(unsigned long base, elf_rela *entry);

static void _relocate(const unsigned long *auxv)
{
	unsigned long phdr_addr = 0;
	unsigned long phdr_num = 0;
	unsigned long phdr_sz = 0;
	unsigned long rela_count = 0;
	unsigned long rela_off = 0;
	elf_phdr *phdr_dyn = NULL;
	unsigned long base;
	int remaining = 3;
	unsigned long i;
	elf_rela *rela;
	elf_dyn *dyn;

	while (remaining) {
		if (!auxv[0] && !auxv[1])
			break;

		switch (auxv[0]){
		case AT_NOTELF:
			return;

		case AT_PHDR:
			phdr_addr = auxv[1];
			remaining--;
			break;

		case AT_PHNUM:
			phdr_num = auxv[1];
			remaining--;
			break;

		/*
		 * Not sure if this is even needed, should match
		 * the size of the program header type?
		 */
		case AT_PHENT:
			phdr_sz = auxv[1];
			remaining--;
			break;
		}

		auxv += 2;
	}

	if (remaining)
		goto failed;

	for (i = 0; i < phdr_num; i++) {
		elf_phdr *phdr = (elf_phdr *)(phdr_addr + (phdr_sz * i));
		switch (phdr->p_type) {
			case PT_INTERP:
				/* Interp was set, we were relocated already?, return */
				return;
			case PT_DYNAMIC:
				phdr_dyn = phdr;
				break;
		}
	}

	if (!phdr_dyn)
		goto failed;

	/*
	 * Everything I could find said that the way to find the base for relocation
	 * should be done by searching for the first PT_LOAD and then using the ofset
	 * of that against the adressed of the program headers. So FIXME.
	 */
	base = phdr_addr - sizeof(elf_ehdr);

	dyn = (elf_dyn *)(base + phdr_dyn->p_offset);
	for (; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case DT_RELA:
			rela_off = dyn->d_un.d_ptr;
			break;
		case DT_RELACOUNT:
			rela_count = dyn->d_un.d_val;
			break;
		}

		/* Got what we came for, exit loop */
		if (rela_off && rela_count)
			break;
	}

	if (!rela_off || !rela_count)
		goto failed;

	rela = (elf_rela *)(base + rela_off);

	for (i = 0; i < rela_count; i++) {
		if (_process_reloc(base, &rela[i]))
			goto failed;
	}

	return;

failed:
	__builtin_trap();
}
#else
static inline void _relocate(const unsigned long *auxv)
{
	/*
	 * Maybe if you build a program that needs relocation
	 * but it's not supported detect that and trap here.
	 * But for now trust that people know what they are doing.
	 */
}
#endif /* NOLIBC_ARCH_HAS_RELOC */

#endif /* _NOLIBC_RELOC_H */
