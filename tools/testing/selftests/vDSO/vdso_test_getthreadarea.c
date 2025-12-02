// SPDX-License-Identifier: GPL-2.0-only
/*
 */

#include <stdint.h>
#include <elf.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/time.h>

#include "../kselftest.h"
#include "parse_vdso.h"
#include "vdso_config.h"
#include "vdso_call.h"

typedef unsigned long(*get_thread_area_t)(void);

const char *symname = "__vdso_get_thread_area";

int main(int argc, char **argv)
{
	const char *version = versions[VDSO_VERSION];
	unsigned long sysinfo_ehdr;
	get_thread_area_t get_thread_area;
	void *thread_area;

	sysinfo_ehdr = getauxval(AT_SYSINFO_EHDR);
	if (!sysinfo_ehdr) {
		printf("AT_SYSINFO_EHDR is not present!\n");
		return KSFT_SKIP;
	}

	vdso_init_from_sysinfo_ehdr(getauxval(AT_SYSINFO_EHDR));

	get_thread_area = (get_thread_area_t)vdso_sym(version, symname);
	if (!get_thread_area) {
		printf("Could not find %s\n", symname);
		return KSFT_SKIP;
	}

	thread_area = (void*) VDSO_CALL(get_thread_area, 0);
	if (thread_area) {
		printf("Thread area %p\n", thread_area);
	} else {
		printf("%s failed\n", symname);
		return KSFT_FAIL;
	}

	return 0;
}
