/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 * 0x00 - config
 *
 *       7      |          6          |                 5             |      4
 *  5541 enable | test pattern enable | yuv transparent colour enable | field invert
 *       3      |          2          |                 1             |      0
 * display mode |     hsync invert    |            vsync invert       |    reset
 *
 */

#define MSTAR_GOP_REG_CONFIG	0x00
#define MSTAR_GOP_REG_DST	0x04
#define MSTAR_GOP_REG_BLINK	0x08
