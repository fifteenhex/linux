/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ELTEC EUROCOM-17 watchdog-reset breadcrumb.
 *
 * A tiny record in battery-backed NVRAM that survives the reset pulse, so the
 * boot after a watchdog reset can say what wedged.  See arch/m68k/eltec/config.c.
 * e17_breadcrumb() marks which suspect "no-DTACK bus access" region CPU0 is in;
 * on a whole-bus hang the hanging access never completes, so the last PHASE
 * written before it names the culprit.
 */
#ifndef _LINUX_E17_BREADCRUMB_H
#define _LINUX_E17_BREADCRUMB_H

#include <linux/types.h>

#define E17_BC_LEN		5	/* MAGIC, HB0, HB1, HB1_SNAP, PHASE */

/* PHASE codes (0 = not in any marked region). */
#define E17_BC_PH_NONE		0x00
#define E17_BC_PH_DRM_BLIT	0x10	/* DRM shadow->VRAM blit           */
#define E17_BC_PH_CD_CONSOLE	0x20	/* CD2401 polled console write     */
#define E17_BC_PH_CD_IACK	0x21	/* CD2401 software IACK window read */
#define E17_BC_PH_CD_DISABLE	0x30	/* CD2401 stop_tx / shutdown       */
#define E17_BC_PH_CD_RX		0x40	/* CD2401 rx-interrupt chip access  */
#define E17_BC_PH_CD_TX		0x50	/* CD2401 tx-interrupt chip access  */
#define E17_BC_PH_CIO_CS	0x60	/* Z8536 CIO clocksource read       */

#if IS_ENABLED(CONFIG_ELTEC_E17)
void e17_breadcrumb(u8 phase);
bool e17_breadcrumb_prev(u8 out[E17_BC_LEN]);
#else
static inline void e17_breadcrumb(u8 phase) { }
static inline bool e17_breadcrumb_prev(u8 out[E17_BC_LEN]) { return false; }
#endif

#endif /* _LINUX_E17_BREADCRUMB_H */
