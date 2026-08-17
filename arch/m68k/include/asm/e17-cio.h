/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ELTEC EUROCOM-17 System CIO (Zilog Z8536) interface.
 *
 * The System CIO's Port C drives the 4-bit 7-seg POST display.  Once the CIO
 * driver (drivers/clocksource/timer-e17-cio.c) has probed it owns Port C and
 * this writes the display.  The earliest POST markers (head.S, smp_secondary.S
 * and config.c's E17_POST during setup_arch) run before the driver exists and
 * necessarily poke Port C ($FEC3.0000) directly.
 */
#ifndef _ASM_M68K_E17_CIO_H
#define _ASM_M68K_E17_CIO_H

#include <linux/types.h>

#ifdef CONFIG_E17_CIO_TIMER
void e17_cio_display(u8 val);
#else
static inline void e17_cio_display(u8 val) { }
#endif

#endif /* _ASM_M68K_E17_CIO_H */
