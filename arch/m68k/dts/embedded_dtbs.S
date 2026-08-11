/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/linkage.h>
#include <asm/page.h>

        .section ".embedded_dtbs","aw"

        .balign PAGE_SIZE

SYM_DATA_START(mvme147_dtb)
        .incbin "arch/m68k/dts/mvme147.dtb"
SYM_DATA_END_LABEL(mvme147_dtb, SYM_L_GLOBAL, mvme147_dtb_end)

SYM_DATA_START(eltec_e17_dtb)
        .incbin "arch/m68k/dts/eltec-e17.dtb"
SYM_DATA_END_LABEL(eltec_e17_dtb, SYM_L_GLOBAL, eltec_e17_dtb_end)
