// SPDX-License-Identifier: GPL-2.0
/*
 *
 */

/*
 * This file handles the architecture-dependent parts of system setup
 */

// new stuff
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/clocksource.h>
//

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/memblock.h>
#include <linux/seq_file.h>
#include <linux/init.h>

#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/machdep.h>
#include <asm/sections.h>

extern void *_dtblob;

unsigned long memory_start;
unsigned long memory_end;

EXPORT_SYMBOL(memory_start);
EXPORT_SYMBOL(memory_end);

static char __initdata cmd_line[COMMAND_LINE_SIZE];

/* machine dependent reboot functions */
void (*mach_reset)(void);
void (*mach_halt)(void);

/*
 * Different cores have different instruction execution timings.
 * The old/traditional 68000 cores are basically all the same, at 16.
 * The ColdFire cores vary a little, their values are defined in their
 * headers. We default to the standard 68000 value here.
 */
#ifndef CPU_INSTR_PER_JIFFY
#define	CPU_INSTR_PER_JIFFY	16
#endif

void __init setup_machine_fdt(void *dt_virt)
{
	if (!dt_virt || !early_init_dt_verify(dt_virt))
		BUG();

	early_init_dt_scan_nodes();

	/* Protect the kernel from itself */
	memblock_reserve(__pa_symbol(&_text),
			 __pa_symbol(&_end) - __pa_symbol(&_text));

	/* Protect the FDT blob from the kernel */
	memblock_reserve(dt_virt, fdt_totalsize(dt_virt));
}

static void __init find_limits(unsigned long *min, unsigned long *max_low,
			       unsigned long *max_high)
{
	*max_low = PFN_DOWN(memblock_get_current_limit());
	*min = PFN_UP(memblock_start_of_DRAM());
	*max_high = PFN_DOWN(memblock_end_of_DRAM());
}

static void __init zone_sizes_init(unsigned long min, unsigned long max_low,
	unsigned long max_high)
{
	unsigned long max_zone_pfn[MAX_NR_ZONES] = { 0 };

#ifdef CONFIG_ZONE_DMA
	max_zone_pfn[ZONE_DMA] = max_low;
#endif
	max_zone_pfn[ZONE_NORMAL] = max_low;

	free_area_init(max_zone_pfn);
}

void __init setup_arch(char **cmdline_p)
{
	// We need to get this somehow better
	setup_machine_fdt(_dtblob);

	setup_initial_init_mm(_stext, _etext, _edata, NULL);

	/* populate cmd_line too for later use, preserving boot_command_line */
	strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = cmd_line;

	memory_start = memblock_start_of_DRAM();
	memory_end = memblock_end_of_DRAM();

	empty_zero_page = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	if (!empty_zero_page)
		BUG();

	pr_info("Flat model support (C) 1998,1999 Kenneth Albanowski, D. Jeff Dionne\n");
	pr_info("Generic DT Machine (C) 2024 Daniel Palmer <daniel@thingy.jp>\n");

	find_limits(&min_low_pfn, &max_low_pfn, &max_pfn);
	zone_sizes_init(min_low_pfn, max_low_pfn, max_pfn);

	unflatten_device_tree();
}

/* machine dependent timer functions */
static void __init setup_dt_sched_init(void)
{
	timer_probe();
}

void (*mach_sched_init)(void) __initdata = setup_dt_sched_init;

/*
 *	Get CPU information for use by the procfs.
 */
static int show_cpuinfo(struct seq_file *m, void *v)
{
	char *cpu, *mmu, *fpu;
	u_long clockfreq;

#if CONFIG_M68000
	cpu = "68000";
#else
	cpu = "dunno!";
#endif
	mmu = "none";
	fpu = "none";
	clockfreq = (loops_per_jiffy * HZ) * CPU_INSTR_PER_JIFFY;

	seq_printf(m, "CPU:\t\t%s\n"
		      "MMU:\t\t%s\n"
		      "FPU:\t\t%s\n"
		      "Clocking:\t%lu.%1luMHz\n"
		      "BogoMips:\t%lu.%02lu\n"
		      "Calibration:\t%lu loops\n",
		      cpu, mmu, fpu,
		      clockfreq / 1000000,
		      (clockfreq / 100000) % 10,
		      (loops_per_jiffy * HZ) / 500000,
		      ((loops_per_jiffy * HZ) / 5000) % 100,
		      (loops_per_jiffy * HZ));

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < NR_CPUS ? ((void *) 0x12345678) : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= show_cpuinfo,
};

