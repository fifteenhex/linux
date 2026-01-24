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
 */

#ifndef _NOLIBC_RELOC_H
#define _NOLIBC_RELOC_H

/*
 * If NOLIBC_WANT_RELOC is not defined decide based on the
 * compiler defining __pie__ or not.
 */
#ifndef NOLIBC_WANT_RELOC
#ifdef __pie__
#define NOLIBC_WANT_RELOC
#endif
#endif

#ifdef NOLIBC_WANT_RELOC
#ifndef _NOLIBC_ARCH_HAS_RELOC
#error "Relocation requested but arch does not support it"
#endif

#include <linux/auxvec.h>
#include "elf.h"

#ifdef _NOLIBC_ARCH_ELF32
#define _nolibc_elf_ehdr		Elf32_Ehdr
#define _nolibc_elf_phdr		Elf32_Phdr
/* 32bit ARM, x86 uses REL instead of RELA */
#ifdef _NOLIBC_ARCH_ELF_REL
#define _nolibc_elf_rel			Elf32_Rel
#else
#define _nolibc_elf_rela		Elf32_Rela
#endif
#define _nolibc_elf_dyn			Elf32_Dyn
#define _nolibc_elf_addr		Elf32_Addr
#define _nolibc_elf_r_type(_x)		ELF32_R_TYPE(_x)
#else
#define _nolibc_elf_ehdr		Elf64_Ehdr
#define _nolibc_elf_phdr		Elf64_Phdr
#define _nolibc_elf_dyn			Elf64_Dyn
#define _nolibc_elf_rela		Elf64_Rela
#define _nolibc_elf_addr		Elf64_Addr
#define _nolibc_elf_r_type(_x)		ELF64_R_TYPE(_x)
#endif

#ifdef _NOLIBC_ARCH_ELF_REL
#define _nolibc_elf_dt_rel_rela		DT_REL
#define _nolibc_elf_dt_rel_rela_count	DT_RELCOUNT
#else
#define _nolibc_elf_dt_rel_rela		DT_RELA
#define _nolibc_elf_dt_rel_rela_count	DT_RELACOUNT
#endif

#ifdef _NOLIBC_ARCH_ELF_REL
/*
 * Your arch needs to provide this to actually handle doing each of the
 * relocations if it uses the REL format.
 */
static int __relocate_rel(unsigned long base, _nolibc_elf_rel *entry);

/* Generic implementation of R_x_RELATIVE for REL */
static __inline__ void __relocate_rel_relative(unsigned long base, _nolibc_elf_rel *entry)
{
	_nolibc_elf_addr *addr;
	int addend;

	addr = (_nolibc_elf_addr *)(base + entry->r_offset);
	addend = *addr;
	*addr = base + addend;
}

static __inline__ int __relocate(unsigned long base,
				 unsigned long rel_off,
				 unsigned long rel_count)
{
	_nolibc_elf_rel *rel = (_nolibc_elf_rel *)(base + rel_off);
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
 * relocations if it uses the RELA format.
 */
static int __relocate_rela(unsigned long base, _nolibc_elf_rela *entry);

/* Generic implementation of R_x_RELATIVE for RELA */
static __inline__ void __relocate_rela_relative(unsigned long base, _nolibc_elf_rela *entry)
{
	_nolibc_elf_addr *addr;

	addr = (_nolibc_elf_addr *) (base + entry->r_offset);
	*addr = (_nolibc_elf_addr) (base + entry->r_addend);
}

static __inline__ int __relocate(unsigned long base,
				 unsigned long rela_off,
				 unsigned long rela_count)
{
	_nolibc_elf_rela *rela = (_nolibc_elf_rela *)(base + rela_off);
	unsigned long i;

	for (i = 0; i < rela_count; i++) {
		if (__relocate_rela(base, &rela[i]))
			return -1;
	}

	return 0;
}
#endif

static __inline__ void _relocate(const unsigned long *auxv)
{
	_nolibc_elf_phdr *phdr_dyn = NULL;
	unsigned long rel_rela_count = 0;
	unsigned long rel_rela_off = 0;
	unsigned long phdr_addr = 0;
	unsigned long phdr_num = 0;
	_nolibc_elf_ehdr *ehdr;
	_nolibc_elf_phdr *phdr;
	_nolibc_elf_dyn *dyn;
	unsigned long base;
	unsigned long i;
	int remaining;

	for (remaining = 2; remaining; ) {
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
		}

		auxv += 2;
	}

	if (remaining)
		goto failed;

	/*
	 * The ELF header and the start of our image in memory
	 * should be the size of the ELF header above the program
	 * headers.
	 */
	base = phdr_addr - sizeof(_nolibc_elf_ehdr);

	/* Check that we are PIE */
	ehdr = (_nolibc_elf_ehdr *) base;
	if (ehdr->e_type != ET_DYN)
		return;

	phdr = (_nolibc_elf_phdr *) phdr_addr;
	for (i = 0; i < phdr_num; i++, phdr++) {
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

	dyn = (_nolibc_elf_dyn *)(base + phdr_dyn->p_vaddr);
	for (remaining = 2; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {
		case _nolibc_elf_dt_rel_rela:
			rel_rela_off = dyn->d_un.d_ptr;
			remaining--;
			break;
		case _nolibc_elf_dt_rel_rela_count:
			rel_rela_count = dyn->d_un.d_val;
			remaining--;
			break;
		}
	}

	if (remaining)
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
}
#endif /* NOLIBC_WANT_RELOC */

#endif /* _NOLIBC_RELOC_H */
