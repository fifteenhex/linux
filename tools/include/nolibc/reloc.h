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
 * this works for me for x86:
 * gcc -nostdlib -fpie -Os -include <path to nolibc.h> \
 * -static-pie -o helloworld helloworld.c
 *
 * For some targets -static-pie doesn't work but setting PIE and
 * then disabling the linker results in a static-pie:
 * gcc -nostdlib -fpie -Os -include <path to nolibc.h> \
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
/* 32bit ARM, x86 uses REL instead of RELA */
#ifdef NOLIBC_ARCH_ELF_REL
#define elf_rel  Elf32_Rel
#else
#define elf_rela Elf32_Rela
#endif
#define elf_dyn  Elf32_Dyn
#define elf_addr Elf32_Addr
#define elf_r_type(_x) ELF32_R_TYPE(_x)
#else
#define elf_ehdr Elf64_Ehdr
#define elf_phdr Elf64_Phdr
#define elf_dyn  Elf64_Dyn
#define elf_rela Elf64_Rela
#define elf_addr Elf64_Addr
#define elf_r_type(_x) ELF64_R_TYPE(_x)
#endif

#ifdef NOLIBC_ARCH_ELF_REL
/*
 * Your arch needs to provide this to actually handle doing each of the
 * relocations if it use the REL format.
 */
static int __relocate_rel(unsigned long base, elf_rel *entry);

/* Generic implementation of R_x_RELATIVE for REL */
#define __relocate_rel_relative(_base, _entry)			\
	do {							\
		elf_addr *_addr;				\
		int addend;					\
								\
		_addr = (elf_addr *)(_base + _entry->r_offset);	\
		addend = *_addr;				\
		*_addr = _base + addend;			\
	} while (0)

static int __relocate(unsigned long base,
		      unsigned long rel_off,
		      unsigned long rel_count)
{
	elf_rel *rel = (elf_rel *)(base + rel_off);
	unsigned long i;

	for (i = 0; i < rel_count; i++) {
		if (__relocate_rel(base, &rel[i]))
			return -1;
	}

	return 0;
}
#else
/*
 * Your arch needs to provide this to actually handle doing each of the
 * relocations if it use the RELA format.
 */
static int __relocate_rela(unsigned long base, elf_rela *entry);

/* Generic implementation of R_x_RELATIVE for RELA */
#define __relocate_rela_relative(_base, _entry)			\
	do {							\
		elf_addr *_addr;				\
								\
		_addr = (elf_addr *)(_base + _entry->r_offset);	\
		*_addr = (elf_addr) (_base + _entry->r_addend);	\
	} while (0)

static int __relocate(unsigned long base,
		      unsigned long rela_off,
		      unsigned long rela_count)
{
	elf_rela *rela = (elf_rela *)(base + rela_off);
	unsigned long i;

	for (i = 0; i < rela_count; i++) {
		if (__relocate_rela(base, &rela[i]))
			return -1;
	}

	return 0;
}
#endif

static void _relocate(const unsigned long *auxv)
{
	unsigned long rel_rela_count = 0;
	unsigned long rel_rela_off = 0;
	unsigned long phdr_addr = 0;
	unsigned long phdr_num = 0;
	unsigned long phdr_sz = 0;
	elf_phdr *phdr_dyn = NULL;
	unsigned long base;
	unsigned long i;
	int remaining;
	elf_ehdr *ehdr;
	elf_dyn *dyn;

	for (remaining = 3; remaining; ) {
		if (!auxv[0] && !auxv[1])
			break;

		switch (auxv[0]) {
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

	/*
	 * Everything I could find said that the way to find the base for relocation
	 * should be done by searching for the first PT_LOAD and then using the ofset
	 * of that against the adressed of the program headers. So FIXME.
	 */
	base = phdr_addr - sizeof(elf_ehdr);

	/* Check that we are PIE */
	ehdr = (elf_ehdr *) base;
	if (ehdr->e_type != ET_DYN)
		return;

	for (i = 0, remaining = 1; (i < phdr_num) && remaining; i++) {
		elf_phdr *phdr = (elf_phdr *)(phdr_addr + (phdr_sz * i));

		switch (phdr->p_type) {
		case PT_INTERP:
			/* Interp was set, we were relocated already?, return */
			return;
		case PT_DYNAMIC:
			phdr_dyn = phdr;
			remaining--;
			break;
		}
	}

	if (!phdr_dyn)
		goto failed;

	dyn = (elf_dyn *)(base + phdr_dyn->p_offset);
	for (; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
#ifdef NOLIBC_ARCH_ELF_REL
		case DT_REL:
			rel_rela_off = dyn->d_un.d_ptr;
			break;
		case DT_RELCOUNT:
			rel_rela_count = dyn->d_un.d_val;
			break;
		}
#else
		case DT_RELA:
			rel_rela_off = dyn->d_un.d_ptr;
			break;
		case DT_RELACOUNT:
			rel_rela_count = dyn->d_un.d_val;
			break;
		}
#endif

		/* Got what we came for, exit loop */
		if (rel_rela_off && rel_rela_count)
			break;
	}

	if (!rel_rela_off || !rel_rela_count)
		goto failed;

	if (__relocate(base, rel_rela_off, rel_rela_count))
		goto failed;

	return;

failed:
	__builtin_trap();
}
#else
static void _relocate(const unsigned long *auxv __attribute__((unused)))
{
	/*
	 * Maybe if you build a program that needs relocation
	 * but it's not supported detect that and trap here.
	 * But for now trust that people know what they are doing.
	 */
}
#endif /* NOLIBC_ARCH_HAS_RELOC */

#endif /* _NOLIBC_RELOC_H */
