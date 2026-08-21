// SPDX-License-Identifier: GPL-2.0-only
/*
 * e17_drm.c - DRM/KMS display driver for the ELTEC Eurocom E17
 *             (EUROCOM-17-5xx) VMEbus MC68040/060 single-board computer.
 *
 * The onboard graphics of the E17 is built from three discrete parts
 * (EUROCOM-17-5xx Hardware Manual, chapter 3.4 "Graphics Interface"):
 *
 *   - a Brooktree Bt445 256x24 colour-palette RAM-DAC with an on-chip
 *     dual PLL pixel-clock synthesizer, at physical 0xfec40000
 *     (manual Table 32 "Register of Bt445");
 *   - a National LM1882 programmable video sync generator plus a
 *     transfer-address generator, together at physical 0xfec48000
 *     (manual Table 33 "Register of Address and Sync Generator");
 *   - 1 MB of dual-ported video RAM at physical 0x0fc00000
 *     (manual Table 31 "Video Address Map", section 3.4.5 "Pixel Model").
 *
 * The register-level programming reproduced here was cross-checked
 * against the manual, the QEMU e17-vid model (hw/display/e17_vid.c in
 * the qemu-e17 tree) and the RMON 3.1.3 firmware mode-set routine
 * (rmon.asm fe80e8c0..fe80ee0e).  See DESIGN.md for the full
 * derivation, the pixel-clock/PLL maths and the memory model.
 *
 * The driver is a complete atomic-modeset KMS driver: one CRTC, one
 * primary plane, one encoder and one connector.  It programs the CRTC
 * timing (LM1882), the pixel clock and pixel format (Bt445) and the
 * scan-out start/pitch (address generator) from scratch for every
 * mode in its table, and drives an 8bpp (C8, palette) framebuffer.
 * fbdev emulation is provided so it acts as the system console,
 * replacing the e17_fb_* hack in arch/m68k/eltec/config.c.
 */

#include <linux/aperture.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#define DRIVER_NAME	"e17drm"
#define DRIVER_DESC	"ELTEC Eurocom E17 onboard video"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

/*
 * The driver programs the analog video timing (Bt445 PLL + LM1882 sync
 * generator + address generator) from scratch on every mode-set: it no longer
 * depends on the mode RMON left set up.  See e17_crtc_atomic_enable() for the
 * PLL-before-LM1882-restart ordering that keeps this stable on real hardware.
 *
 * e17_drm.linear_pitch=1 selects the linear/panning address-generator layout
 * (TRINC-padded pitch, SPLIT clear) instead of the default packed-line layout.
 * Packed keeps the pitch == visible width, halves the VRAM footprint and is the
 * only layout that leaves room to double-buffer; the linear path is for
 * experiments only.
 */
static bool e17_linear_pitch;
module_param_named(linear_pitch, e17_linear_pitch, bool, 0444);
MODULE_PARM_DESC(linear_pitch,
	"use linear TRINC-padded pitch instead of the packed (RMON-parity) layout (default 0)");

/*
 * DRM vblank.  Default OFF.  When enabled we init vblank and drive it from drm's
 * hrtimer (see e17_enable_vblank).  On real hardware that is not yet reliable:
 * this board has no high-res clockevent, so the vblank hrtimer is low-res (tied
 * to the periodic tick), and the fbdev damage worker waits for a vblank on every
 * update (drm_fb_helper_fb_dirty -> drm_client_modeset_wait_for_vblank).  When
 * the polled console holds interrupts off during heavy output the tick -- and
 * thus the vblank -- stalls, the wait times out, and its WARN feeds more console
 * output: a self-amplifying storm.  Until there is a real clockevent (or the
 * fbdev wait is avoided) keep vblank behind this flag; the default build stays on
 * the rock-solid counting-only frame IRQ.  Enable with e17_drm.vblank=1.
 */
static bool e17_vblank;
module_param_named(vblank, e17_vblank, bool, 0444);
MODULE_PARM_DESC(vblank,
	"Enable DRM vblank via hrtimer (default 0; not yet reliable on hardware)");

/*
 * Request the LM1882 frame interrupt (VIC LIRQ4).  It is counting-only (vblank
 * is off), so nothing depends on it -- but a mislatched LM1882 frame/cursor
 * output can turn LIRQ4 (a plain handle_simple_irq with no HW ack) into a storm
 * that hard-locks the board.  Default OFF while that is under suspicion; with it
 * off the line is left masked and no frame interrupt is delivered.  Turn it back
 * on with e17_drm.frame_irq=1 (also needed once vblank is wired to it).
 */
static bool e17_frame_irq;
module_param_named(frame_irq, e17_frame_irq, bool, 0444);
MODULE_PARM_DESC(frame_irq,
	"request the LM1882 frame interrupt on LIRQ4 (counting-only; default 0)");

/*
 * VRAM mapping mode (performance).  m68k has no real ioremap_wc, so the default
 * (ioremap_wc) degrades to cache-inhibited SERIALIZED access -- every store is a
 * stalled, non-posted single bus cycle (~6-12 MB/s), so a full 800x600x8 frame
 * costs 40-80 ms.  e17_drm.vram_wt=1 maps the scan-out buffer WRITE-THROUGH
 * cacheable instead: posted single writes (~20-25 MB/s) and cached reads, ~2-4x
 * faster blits.  Default OFF because sustained VRAM writes under dual-68040 +
 * active scan-out are exactly the historical "bus hard-lock" territory
 * (config.c) and this needs a real-hardware soak before it can be the default.
 */
static bool e17_vram_wt;
module_param_named(vram_wt, e17_vram_wt, bool, 0444);
MODULE_PARM_DESC(vram_wt,
	"map VRAM write-through cacheable instead of cache-inhibited serialized "
	"(faster blits; default 0 -- needs a hardware soak first)");

/*
 * At probe, micro-benchmark VRAM write bandwidth (memcpy_toio into the
 * non-scanned-out top half of VRAM) and log MB/s, so the effect of vram_wt can
 * be measured on real hardware.  Default off.
 */
static bool e17_vram_bench;
module_param_named(vram_bench, e17_vram_bench, bool, 0444);
MODULE_PARM_DESC(vram_bench,
	"at probe, measure and log VRAM write bandwidth (default 0)");

/*
 * Use the 68040 move16 burst instruction for the shadow->VRAM blit instead of
 * plain memcpy_toio.  move16 moves an aligned 16-byte block per op (burst write,
 * ~40-53 MB/s per the manual vs ~20-25 MB/s for posted singles).  Only the
 * full-width, matching-pitch, 16-byte-aligned case is contiguous and alignable
 * (always true for the default packed C8 layout, pitch 800); everything else
 * falls back to memcpy_toio.  Default off pending a hardware check.
 */
static bool e17_move16;
module_param_named(move16, e17_move16, bool, 0444);
MODULE_PARM_DESC(move16,
	"blit shadow->VRAM with move16 bursts where aligned (default 0)");


/*
 * ---------------------------------------------------------------------
 * Bt445 RAM-DAC (register bank at 0xfec40000, byte-wide)
 * ---------------------------------------------------------------------
 *
 * The Bt445 uses an indirect addressing scheme (manual 3.4.2): the
 * indirect register address is written to BT_ADR (+0), then the target
 * register is read/written through one of the "direct" byte ports; the
 * address auto-increments after each access.  The direct ports are:
 *
 *   +0  BT_ADR   indirect address / palette write address
 *   +1  BT_CLUT  primary colour palette RAM data (R,G,B, modulo-3)
 *   +2  command register file (indirect: 0=IDR .. 6=CR0, 7=TR0)
 *   +5  colour width/position register file (R/G/B/overlay/cursor)
 *   +6  PLL / pixel-format register file (indirect 0x00..0x0f)
 *   +7  cursor-colour register file
 *
 * The ID register (IDR, indirect 0 read through +2) reads 0x3a on this
 * board and is used for probing.
 */
#define BT_ADR			0x0	/* indirect address / palette addr */
#define BT_CLUT			0x1	/* palette data (R,G,B autoinc)    */
#define BT_CMD			0x2	/* command-register file / ID      */
#define BT_COLWIDTH		0x5	/* colour width/position file      */
#define BT_PLLFMT		0x6	/* PLL + pixel-format file         */
#define BT_CURCOL		0x7	/* cursor-colour file              */

#define BT_ID_VALUE		0x3a	/* IDR read value on the E17       */

/* Indirect registers within the +2 command file */
#define BT_IDR			0x00	/* ID register (read)              */
#define BT_CR0			0x06	/* Command Register 0              */

/* Indirect registers within the +5 colour width/position file */
#define BT_RMSBP		0x00	/* Red   MSB position              */
#define BT_RWC			0x01	/* Red   width control             */
#define BT_RDEC			0x02	/* Red   display-enable control    */
#define BT_RBER			0x03	/* Red   blink-enable register     */
#define BT_GMSBP		0x08	/* Green MSB position              */
#define BT_GWC			0x09
#define BT_GDEC			0x0a
#define BT_GBER			0x0b
#define BT_BMSBP		0x10	/* Blue  MSB position              */
#define BT_BWC			0x11
#define BT_BDEC			0x12
#define BT_BBER			0x13

/* Indirect registers within the +6 PLL / pixel-format file */
#define BT_CR1			0x01	/* Command Register 1              */
#define BT_DOCR			0x02	/* Digital Output Control          */
#define BT_VCCR			0x03	/* VIDCLK Cycle Control            */
#define BT_PLLR0		0x05	/* PLL Rate Register 0  (M)        */
#define BT_PLLR1		0x06	/* PLL Rate Register 1  (N|postdiv)*/
#define BT_PLLCR		0x07	/* PLL Control Register            */
#define BT_PLCR			0x08	/* Pixel Load Control              */
#define BT_PPSPR		0x09	/* Pixel Port Start Position       */
#define BT_PFCR			0x0a	/* Pixel Format Control            */
#define BT_MPXRR		0x0b	/* MPX Rate Register               */
#define BT_PDCR			0x0d	/* Pixel Depth Control             */
#define BT_PBWC			0x0f	/* Palette Bypass Width Control    */

/*
 * ---------------------------------------------------------------------
 * Address & sync generator (register bank at 0xfec48000, byte-wide)
 * ---------------------------------------------------------------------
 * manual Table 33.
 */
#define AG_TRLSB		0x0	/* transfer (top-of-screen) addr LSB */
#define AG_TRMIB		0x1	/* ..middle byte                     */
#define AG_TRMSB		0x2	/* ..most significant byte           */
#define AG_TRINC		0x4	/* line pitch, in 128-byte units     */
#define AG_GRMODE		0x5	/* graphics mode register            */
#define AG_ADR1882		0x6	/* LM1882 indirect address           */
#define AG_DAT1882		0x7	/* LM1882 data (16-bit, low then high)*/

/* GRMODE bits (manual Table 33 / section 3.4.3) */
#define GRMODE_SPLIT		BIT(0)	/* packed-line (split) transfer     */
#define GRMODE_DISOVERL		BIT(1)	/* 1: 1/2/4/8bpp, 0: 8bpp+4b overlay*/
#define GRMODE_PIXDEL		BIT(2)	/* delay digital pixel data         */
#define GRMODE_INTERL		BIT(3)	/* interlaced                        */
#define GRMODE_ENTRA		BIT(4)	/* enable address-transfer cycles   */
#define GRMODE_COMPSYN		BIT(5)	/* composite sync                   */

/* Plain linear 8bpp, non-interlaced, address-transfer enabled. */
#define GRMODE_LINEAR_8BPP	(GRMODE_DISOVERL | GRMODE_ENTRA)

/*
 * The GRMODE value RMON leaves programmed and that we inherit by default:
 * packed-line (SPLIT, TRINC in 512-byte units) 8bpp, pixel-delay set,
 * address-transfer enabled -- 0x17, captured from RMON's mode-set.  We must
 * re-assert exactly this (not bare LINEAR_8BPP=0x12) when unblanking the
 * inherited mode: dropping SPLIT while TRINC is still 4 switches the address
 * generator to synchronous transfer against a 512-byte stride and shears the
 * picture.  The address generator is write-only, so track the live value in a
 * shadow (e17_device.grmode) and toggle only ENTRA to blank/unblank.
 */
#define GRMODE_INHERIT_8BPP	(GRMODE_SPLIT | GRMODE_DISOVERL | \
				 GRMODE_PIXDEL | GRMODE_ENTRA)

#define E17_LM1882_NREGS	19	/* timing registers 0..18           */
#define E17_LM1882_RESTART	22	/* ADDRDEC Vectored Restart address */
#define E17_LM1882_RESUME	19	/* dummy non-ADDRDEC addr to resume */

/*
 * Packed-line (SPLIT) transfer: TRINC holds the transfer burst size in 128-byte
 * units, which RMON programs to 512 (=4) regardless of the visible width; the
 * scan-out stride is the visible line length, not TRINC*128 (manual 3.4.7).
 */
#define E17_TRINC_PACKED	4

/* PLL reference (manual 3.4.7: "PLL reference clock is 25.175 MHz"). */
#define E17_PLL_REF_HZ		25175000u
/*
 * Time to let the Bt445 pixel-clock PLL relock after reprogramming, before the
 * LM1882 (whose VIDCLK is derived from it, manual 3.4.7) is restarted onto it.
 * Generous; a mode-set is rare.  See e17_crtc_atomic_enable().
 */
#define E17_PLL_LOCK_US		2000u
#define E17_PLL_VCO_MIN_HZ	75000000u	/* manual 3.4.7 */
#define E17_PLL_VCO_MAX_HZ	150000000u	/* manual 3.4.7 */

/* VRAM: 1 MB of dual-ported video memory (manual Table 31, 3.4.5). */
#define E17_VRAM_SIZE		SZ_1M

/*
 * A mode: the DRM description exposed to userspace plus the private
 * hardware parameters (pixel clock and the LM1882 timing register file)
 * needed to program it.  The LM1882 register values are taken from the
 * RMON firmware's per-mode tables (see E17-NOTES.md "CRTC timing
 * register table"); they encode the horizontal/vertical sync and blank
 * positions and the sync polarities for each mode.
 */
struct e17_mode {
	struct drm_display_mode mode;
	u32 dotclock_hz;
	u16 lm1882[E17_LM1882_NREGS];
};

/*
 * The mode table.  Index 0 is the preferred/default mode and matches
 * exactly what RMON programs and what the QEMU model expects
 * ("800x600 35kHz 56Hz, 8bpp").  Additional entries are the other
 * progressive SVGA/VGA modes from the manual's Table 34, with the
 * LM1882 register values captured from RMON for each.
 *
 * DRM display-mode timings (below) are the standard VESA numbers for
 * userspace; the actual scan-out timing is produced by the LM1882
 * register file, so the two need only agree on active size and refresh.
 */
static const struct e17_mode e17_modes[] = {
	{
		/* 800x600 @ 56 Hz, 36 MHz - RMON default, QEMU-verified */
		.mode = {
			DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 36000,
				 800, 824, 896, 1024, 0,
				 600, 601, 603, 625, 0,
				 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
		},
		.dotclock_hz = 36000000,
		.lm1882 = { 0x060e, 0x000d, 0x0031, 0x006f, 0x01fe, 0x0002,
			    0x0004, 0x001a, 0x0271, 0x002c, 0x01c1, 0x0001,
			    0x0008, 0x0000, 0x0000, 0x006e, 0x01fd, 0x0001,
			    0x0002 },
	},
	{
		/* 800x600 @ 60 Hz, 40 MHz (manual Table 34) */
		.mode = {
			DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 40000,
				 800, 840, 968, 1056, 0,
				 600, 601, 605, 628, 0,
				 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
		},
		.dotclock_hz = 40000000,
		.lm1882 = { 0x060e, 0x0015, 0x0055, 0x0081, 0x0210, 0x0002,
			    0x0006, 0x001d, 0x0274, 0x0035, 0x01d1, 0x0001,
			    0x0008, 0x0000, 0x0000, 0x0080, 0x020f, 0x0001,
			    0x0002 },
	},
	{
		/* 640x480 @ 60 Hz, 25.175 MHz (manual Table 34) */
		.mode = {
			DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175,
				 640, 656, 752, 800, 0,
				 480, 490, 492, 525, 0,
				 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
		},
		.dotclock_hz = 25175000,
		.lm1882 = { 0x060e, 0x0009, 0x0039, 0x0051, 0x0190, 0x000b,
			    0x000d, 0x002e, 0x020d, 0x0021, 0x0161, 0x0006,
			    0x000c, 0x0000, 0x0000, 0x0050, 0x018f, 0x0001,
			    0x0002 },
	},
	{
		/* 1024x768 @ 60 Hz, 65 MHz (manual Table 34) */
		.mode = {
			DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000,
				 1024, 1048, 1184, 1344, 0,
				 768, 771, 777, 806, 0,
				 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
		},
		.dotclock_hz = 65000000,
		.lm1882 = { 0x060e, 0x0007, 0x0029, 0x0051, 0x0150, 0x0004,
			    0x000a, 0x0027, 0x0326, 0x0018, 0x012f, 0x0002,
			    0x000c, 0x0000, 0x0000, 0x0050, 0x014f, 0x0001,
			    0x0002 },
	},
};

struct e17_device {
	struct drm_device dev;

	void __iomem *dac;	/* Bt445 register bank (0xfec40000)    */
	void __iomem *ag;	/* address+sync generator (0xfec48000) */
	struct iosys_map vram;	/* scan-out buffer (0x0fc00000, iomem) */
	unsigned int pitch;	/* current hardware line pitch (bytes) */
	u8 grmode;		/* shadow of the write-only AG_GRMODE register */

	/*
	 * The timing (PLL/LM1882/addrgen) currently programmed into the hardware.
	 * atomic_enable reprograms it only when the requested mode differs, so the
	 * costly from-scratch sequence (and its monitor re-lock) runs on a real
	 * mode change, not on every DPMS-on / VT-switch / client-exit re-enable.
	 */
	struct drm_display_mode active_mode;
	bool active_valid;

	/* Frame-IRQ screaming-source guard (see e17_frame_isr). */
	unsigned long frame_jiffy;
	u32 frame_per_jiffy;
	bool frame_stormed;

	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	u32 formats[2];
};

static inline struct e17_device *to_e17(struct drm_device *dev)
{
	return container_of(dev, struct e17_device, dev);
}

/*
 * Hardware line pitch.  The address generator's increment register
 * (AG_TRINC) counts in 128-byte units (manual Table 33), so the pitch
 * is the byte-per-line count rounded up to a multiple of 128.  For
 * 800x600x8 this yields 896 bytes/line - the value RMON also computes
 * (rmon.asm fe80e9a4..fe80e9c2).
 */
static unsigned int e17_pitch(unsigned int width, unsigned int cpp)
{
	return ALIGN(width * cpp, 128);
}

/*
 * ---------------------------------------------------------------------
 * Low-level register helpers
 * ---------------------------------------------------------------------
 */

/* Write one Bt445 indirect register through the given direct port. */
static void bt445_write(struct e17_device *e17, u8 index, u8 port, u8 val)
{
	writeb(index, e17->dac + BT_ADR);
	writeb(val, e17->dac + port);
}

static u8 bt445_read_id(struct e17_device *e17)
{
	writeb(BT_IDR, e17->dac + BT_ADR);
	return readb(e17->dac + BT_CMD);
}

/* Load one palette entry (BT_ADR then three modulo-3 data bytes). */
static void e17_set_palette(struct drm_crtc *crtc, unsigned int index,
			    u16 red, u16 green, u16 blue)
{
	struct e17_device *e17 = to_e17(crtc->dev);

	writeb(index, e17->dac + BT_ADR);
	writeb(red >> 8, e17->dac + BT_CLUT);
	writeb(green >> 8, e17->dac + BT_CLUT);
	writeb(blue >> 8, e17->dac + BT_CLUT);
}

/*
 * Pixel-clock PLL.
 *
 * The Bt445 synthesizes the pixel clock as
 *
 *      pixel_clock = (M / N) * 25.175 MHz / postdiv
 *
 * where the VCO frequency M/N * 25.175 MHz must lie in 75..150 MHz
 * (manual 3.4.7) and postdiv is 1/2/4/8.  RMON (rmon.asm fe80ea10..
 * fe80eb1c) picks postdiv to bring the VCO into range, then searches
 * M in [24,63] and N in [4,15] for the closest match; we reproduce
 * that search verbatim so the programmed clock is bit-identical to the
 * firmware's.  The three PLL registers are then:
 *
 *      PLLR0 = M
 *      PLLR1 = (N - 1) | (log2(postdiv) << 6)
 *      PLLCR = (7 - (7 * (VCO - 75MHz) / 75MHz)) | 0xf0
 */
static void e17_program_pll(struct e17_device *e17, u32 dotclock_hz)
{
	u32 postdiv, target_vco, best_vco = 0, best_diff = U32_MAX;
	unsigned int m, n, best_m = 24, best_n = 4, log2_pd;
	u32 x, t;

	if (dotclock_hz > 74999999)
		postdiv = 1;
	else if (dotclock_hz > 37499999)
		postdiv = 2;
	else if (dotclock_hz > 18749999)
		postdiv = 4;
	else
		postdiv = 8;

	target_vco = dotclock_hz * postdiv;

	for (m = 24; m <= 63; m++) {
		for (n = 4; n <= 15; n++) {
			u32 vco = (u32)div_u64((u64)m * E17_PLL_REF_HZ, n);
			u32 diff = (vco > target_vco) ? vco - target_vco
						      : target_vco - vco;

			if (diff < best_diff) {
				best_diff = diff;
				best_vco = vco;
				best_m = m;
				best_n = n;
			}
		}
	}

	log2_pd = ilog2(postdiv);	/* 0,1,2,3 for 1,2,4,8 */

	/* Clamp for the PLLCR fraction; the search keeps VCO near range. */
	x = (best_vco > E17_PLL_VCO_MIN_HZ) ? best_vco - E17_PLL_VCO_MIN_HZ : 0;
	t = min_t(u32, (7u * x) / E17_PLL_VCO_MIN_HZ, 7u);

	bt445_write(e17, BT_PLLR0, BT_PLLFMT, best_m);
	bt445_write(e17, BT_PLLR1, BT_PLLFMT, (best_n - 1) | (log2_pd << 6));
	bt445_write(e17, BT_PLLCR, BT_PLLFMT, (7u - t) | 0xf0);

	drm_dbg_kms(&e17->dev,
		    "PLL: dotclock=%u postdiv=%u -> M=%u N=%u vco=%u\n",
		    dotclock_hz, postdiv, best_m, best_n, best_vco);
}

/*
 * Program the Bt445 for plain 8bpp (C8) indexed scan-out.
 *
 * The constants below are the 8-bit-per-pixel case of the general
 * RMON setup (rmon.asm fe80eab0..fe80ede6): each colour gun takes the
 * full 8 palette bits (MSB position 7, width 8, display-enable 0xff),
 * the pixel-depth and pixel-format registers select 8bpp, and no
 * overlay/cursor/blink is used.  See DESIGN.md for the full mapping.
 */
static void e17_program_bt445(struct e17_device *e17, u32 dotclock_hz)
{
	e17_program_pll(e17, dotclock_hz);

	/* Command registers */
	bt445_write(e17, BT_CR0, BT_CMD, 0x63);		/* CR0 (via +2)     */
	bt445_write(e17, BT_CR1, BT_PLLFMT, 0x44);	/* CR1: 8bpp, no ovl*/
	bt445_write(e17, BT_PPSPR, BT_PLLFMT, 0x40);	/* pixel port start */

	/* Red/Green/Blue: MSB position 7, width 8, all outputs enabled */
	bt445_write(e17, BT_RMSBP, BT_COLWIDTH, 0x07);
	bt445_write(e17, BT_RWC, BT_COLWIDTH, 0x08);
	bt445_write(e17, BT_RDEC, BT_COLWIDTH, 0xff);
	bt445_write(e17, BT_RBER, BT_COLWIDTH, 0x00);
	bt445_write(e17, BT_GMSBP, BT_COLWIDTH, 0x07);
	bt445_write(e17, BT_GWC, BT_COLWIDTH, 0x08);
	bt445_write(e17, BT_GDEC, BT_COLWIDTH, 0xff);
	bt445_write(e17, BT_GBER, BT_COLWIDTH, 0x00);
	bt445_write(e17, BT_BMSBP, BT_COLWIDTH, 0x07);
	bt445_write(e17, BT_BWC, BT_COLWIDTH, 0x08);
	bt445_write(e17, BT_BDEC, BT_COLWIDTH, 0xff);
	bt445_write(e17, BT_BBER, BT_COLWIDTH, 0x00);

	/* Pixel format / depth (8bpp, no palette bypass) */
	bt445_write(e17, BT_PBWC, BT_PLLFMT, 0x00);
	bt445_write(e17, BT_PFCR, BT_PLLFMT, 0x02);
	bt445_write(e17, BT_PDCR, BT_PLLFMT, 0x08);
	bt445_write(e17, BT_VCCR, BT_PLLFMT, dotclock_hz > 59999999 ? 0x03 : 0x01);
	bt445_write(e17, BT_PLCR, BT_PLLFMT, 0x0c);
	bt445_write(e17, BT_DOCR, BT_PLLFMT, 0x14);
	bt445_write(e17, BT_MPXRR, BT_PLLFMT, 0x03);

	/* Finally, enable the pixel pipeline (CR1 bit 0). */
	writeb(BT_CR1, e17->dac + BT_ADR);
	writeb(0x45, e17->dac + BT_PLLFMT);
}

/*
 * Load the LM1882 sync-generator timing file (regs 0..18), then Vectored Restart
 * and RESUME.
 *
 * Address 22 (Vectored Restart) reloads the counters but FREEZES the sync
 * outputs and stops the internal clock; the device stays frozen until a
 * non-ADDRDEC address is loaded (LM1882 datasheet, ADDRDEC Logic).  RMON writes
 * the restart, one data byte, then dummy address 19 to resume counting
 * (rmon.asm fe80e93c..fe80e94a).  The resume write is essential -- omitting it
 * (as this driver originally did) leaves the generator frozen, so there is no
 * H/V sync at all and the monitor reports "no signal".
 */
static void e17_program_lm1882(struct e17_device *e17, const u16 *regs)
{
	unsigned int i;

	writeb(0, e17->ag + AG_ADR1882);		/* index := 0 */
	for (i = 0; i < E17_LM1882_NREGS; i++) {
		writeb(regs[i] & 0xff, e17->ag + AG_DAT1882);	/* low  */
		writeb(regs[i] >> 8, e17->ag + AG_DAT1882);	/* high */
	}
	writeb(E17_LM1882_RESTART, e17->ag + AG_ADR1882);
	writeb(0, e17->ag + AG_DAT1882);
	writeb(E17_LM1882_RESUME, e17->ag + AG_ADR1882);	/* resume: unfreeze */
}

/*
 * Program the transfer-address generator: scan-out start 0, the transfer-burst
 * size (TRINC, 128-byte units) and the graphics mode/enable (GRMODE).  Caller
 * passes the raw TRINC and GRMODE values so the packed (RMON-parity) and linear
 * (panning experiment) paths can differ.
 */
static void e17_program_addrgen(struct e17_device *e17, u8 trinc, u8 grmode)
{
	writeb(0x00, e17->ag + AG_TRLSB);	/* top-of-screen address 0 */
	writeb(0x00, e17->ag + AG_TRMIB);
	writeb(0x00, e17->ag + AG_TRMSB);
	writeb(trinc, e17->ag + AG_TRINC);
	writeb(grmode, e17->ag + AG_GRMODE);
}

static const struct e17_mode *e17_find_mode(const struct drm_display_mode *m)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(e17_modes); i++) {
		if (e17_modes[i].mode.hdisplay == m->hdisplay &&
		    e17_modes[i].mode.vdisplay == m->vdisplay &&
		    abs((int)e17_modes[i].mode.clock - (int)m->clock) < 100)
			return &e17_modes[i];
	}
	return &e17_modes[0];
}

/*
 * ---------------------------------------------------------------------
 * Primary plane (shadow-buffered: blit system-memory FB into VRAM)
 * ---------------------------------------------------------------------
 */

static int e17_plane_atomic_check(struct drm_plane *plane,
				  struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

/*
 * Copy len bytes from system memory into VRAM using 68040 move16 bursts.
 * Caller guarantees src, dst and len are all 16-byte aligned and len > 0.
 */
static void e17_move16_toio(void __iomem *dst, const void *src, size_t len)
{
	void *d = (void __force *)dst;
	const void *s = src;
	unsigned long n = len / 16;

	asm volatile(".chip 68040\n"
		     "1:\tmove16 %1@+,%0@+\n\t"
		     ".chip 68k\n\t"
		     "subq.l #1,%2\n\t"
		     "jne 1b\n\t"
		     : "+a" (d), "+a" (s), "+d" (n)
		     :
		     : "memory");
}

static void e17_plane_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct e17_device *e17 = to_e17(plane->dev);
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow =
		to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	unsigned int dst_pitch = e17->pitch;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	int ret, idx;

	if (!fb)
		return;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return;

	if (!drm_dev_enter(&e17->dev, &idx))
		goto out_end_access;

	drm_atomic_helper_damage_iter_init(&iter, old_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct iosys_map dst = e17->vram;
		struct drm_rect dst_clip = plane_state->dst;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		/*
		 * move16 fast path: a full-width clip with matching, 16-aligned
		 * pitch is a single contiguous 16-aligned run in both buffers
		 * (true for the packed C8 layout), so burst it in one go.
		 */
		if (e17_move16 &&
		    dst_clip.x1 == plane_state->dst.x1 &&
		    dst_clip.x2 == plane_state->dst.x2 &&
		    dst_pitch == fb->pitches[0] && !(dst_pitch & 15)) {
			size_t off = (size_t)dst_clip.y1 * dst_pitch;
			size_t len = (size_t)drm_rect_height(&dst_clip) * dst_pitch;

			e17_move16_toio(e17->vram.vaddr_iomem + off,
					shadow->data[0].vaddr + off, len);
			continue;
		}

		iosys_map_incr(&dst,
			       drm_fb_clip_offset(dst_pitch, fb->format, &dst_clip));
		drm_fb_memcpy(&dst, &dst_pitch, shadow->data, fb, &dst_clip);
	}

	drm_dev_exit(idx);
out_end_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static const struct drm_plane_helper_funcs e17_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = e17_plane_atomic_check,
	.atomic_update = e17_plane_atomic_update,
};

static const struct drm_plane_funcs e17_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/*
 * ---------------------------------------------------------------------
 * CRTC
 * ---------------------------------------------------------------------
 */

static void e17_crtc_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
{
	struct e17_device *e17 = to_e17(crtc->dev);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	const struct e17_mode *hw = e17_find_mode(mode);
	bool reprogram = !e17->active_valid ||
			 e17->active_mode.hdisplay != mode->hdisplay ||
			 e17->active_mode.vdisplay != mode->vdisplay ||
			 e17->active_mode.clock    != mode->clock;

	if (reprogram) {
		/*
		 * Full mode-set from scratch (no longer inherit RMON's).  ORDER IS
		 * CRITICAL: the LM1882 sync generator is clocked by VIDCLK, which the
		 * Bt445 PLL derives from the pixel clock (manual 3.4.7).  Program the
		 * Bt445 (PLL + format) FIRST, let the PLL relock, then issue the
		 * LM1882's Vectored Restart LAST -- the LM1882 datasheet requires the
		 * restart to be the final reprogramming step so the counters start at
		 * pixel 1 on a clock that will not move again.
		 *
		 * RMON uses the literal opposite order but only survives it at cold
		 * boot, where VIDCLK does not exist until RMON programs the PLL -- so
		 * RMON's LM1882 also gets its first clock under the final, stable
		 * clock.  On a live board (RMON's PLL already locked) resuming the
		 * LM1882 and THEN relocking the PLL under it leaves the cursor/frame
		 * comparator (wired to LIRQ4) mislatched: a frame-IRQ storm that
		 * hard-locks the board, even though the sync comparators recover a
		 * good picture.
		 *
		 * Default to RMON's packed-line layout (pitch == visible width,
		 * TRINC=4/512-byte burst, GRMODE 0x17); the linear/panning layout
		 * (896 pitch, GRMODE 0x12) is retained behind e17_drm.linear_pitch.
		 */
		e17_program_bt445(e17, hw->dotclock_hz);
		udelay(E17_PLL_LOCK_US);	/* VIDCLK must be stable before the LM1882 restart */
		if (e17_linear_pitch) {
			e17->pitch = e17_pitch(mode->hdisplay, 1); /* 896 */
			e17->grmode = GRMODE_LINEAR_8BPP;
			e17_program_addrgen(e17, e17->pitch / 128, e17->grmode);
		} else {
			e17->pitch = mode->hdisplay;		/* packed: 800 */
			e17->grmode = GRMODE_INHERIT_8BPP;	/* 0x17 */
			e17_program_addrgen(e17, E17_TRINC_PACKED, e17->grmode);
		}
		e17_program_lm1882(e17, hw->lm1882); /* Vectored Restart LAST, stable VIDCLK */

		drm_mode_copy(&e17->active_mode, mode);
		e17->active_valid = true;

		/* Fresh mode: default palette until fbcon/userspace sets gamma. */
		drm_crtc_fill_palette_8(crtc, e17_set_palette);
	} else {
		/*
		 * Same mode already programmed: the PLL/LM1882 timing and the palette
		 * are intact across a DPMS-off / VT-switch / client-exit blank (only
		 * atomic_disable's clearing of the transfer-enable took the picture
		 * away).  Just re-assert scan-out -- no PLL relock, no LM1882 restart,
		 * so no monitor re-lock and none of the reprogram's risk.
		 */
		e17->grmode |= GRMODE_ENTRA;
		writeb(e17->grmode, e17->ag + AG_GRMODE);
	}

	if (e17_vblank)
		drm_crtc_vblank_on(crtc);	/* start vblank accounting (hrtimer) */

	drm_dbg_kms(&e17->dev, "enabled %dx%d, pitch=%u (%s)\n",
		    mode->hdisplay, mode->vdisplay, e17->pitch,
		    reprogram ? "reprogrammed" : "scan-out re-asserted");
}

static void e17_crtc_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_commit *state)
{
	struct e17_device *e17 = to_e17(crtc->dev);

	/* Flush pending vblank events and stop the vblank timer before blanking. */
	if (e17_vblank)
		drm_crtc_vblank_off(crtc);

	/*
	 * Blank by clearing only the transfer-enable (ENTRA) in the shadow; keep
	 * the mode bits (SPLIT/PIXDEL) so the matching enable can restore the
	 * exact inherited GRMODE rather than a bare 0x12 that would shear.
	 */
	e17->grmode &= ~GRMODE_ENTRA;
	writeb(e17->grmode, e17->ag + AG_GRMODE);
}

static void e17_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	/*
	 * Palette (C8) is exposed through the CRTC gamma LUT: fbcon and
	 * userspace program the colour map, which arrives here as the
	 * gamma_lut blob.  Load it into the Bt445 CLUT.
	 */
	if (crtc_state->enable && crtc_state->color_mgmt_changed) {
		if (crtc_state->gamma_lut)
			drm_crtc_load_palette_8(crtc,
						crtc_state->gamma_lut->data,
						e17_set_palette);
		else
			drm_crtc_fill_palette_8(crtc, e17_set_palette);
	}

	/*
	 * Complete the commit's flip event immediately.  The shadow-plane blit is
	 * synchronous (the pixels are already in VRAM by now), so there is nothing
	 * to defer to a real frame; sending here -- instead of arming it on a vblank
	 * -- is what keeps a commit from ever blocking.  See e17_commit_tail() for
	 * why this driver deliberately does not wait for vblank.  When vblank is off
	 * the commit tail's fake-vblank path delivers the event instead, so only send
	 * it here when vblank is actually enabled.
	 */
	if (e17_vblank && crtc_state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc_state->event);
		crtc_state->event = NULL;
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_crtc_helper_funcs e17_crtc_helper_funcs = {
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_enable = e17_crtc_atomic_enable,
	.atomic_disable = e17_crtc_atomic_disable,
	.atomic_flush = e17_crtc_atomic_flush,
};

/*
 * Vblank source: a kernel hrtimer, NOT the frame hard-IRQ.
 *
 * The real LM1882 frame interrupt (VIC LIRQ4, CPU IPL 5) is left as a
 * counting-only ISR (see e17_frame_isr).  Driving drm_crtc_handle_vblank() from
 * that hard-IRQ on this first-ever m68k SMP board wedged the console: the
 * cross-CPU wake of a waiter blocked in drm_atomic_helper_wait_for_vblanks()
 * timed out and its WARNs collapsed fbcon throughput.  Using drm's core hrtimer
 * vblank keeps the vblank handling in timer context, co-located with any waiter,
 * and off the shared level-5 line.  Combined with e17_commit_tail() (which does
 * not wait for vblank at all), no commit ever depends on a cross-CPU wake.
 */
static int e17_enable_vblank(struct drm_crtc *crtc)
{
	return drm_crtc_vblank_start_timer(crtc);
}

static void e17_disable_vblank(struct drm_crtc *crtc)
{
	drm_crtc_vblank_cancel_timer(crtc);
}

static const struct drm_crtc_funcs e17_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = e17_enable_vblank,
	.disable_vblank = e17_disable_vblank,
};

/*
 * ---------------------------------------------------------------------
 * Encoder + connector
 * ---------------------------------------------------------------------
 */

static const struct drm_encoder_funcs e17_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int e17_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	unsigned int i, count = 0;

	for (i = 0; i < ARRAY_SIZE(e17_modes); i++) {
		struct drm_display_mode *m =
			drm_mode_duplicate(dev, &e17_modes[i].mode);

		if (!m)
			continue;
		if (i == 0)
			m->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, m);
		count++;
	}

	return count;
}

static const struct drm_connector_helper_funcs e17_connector_helper_funcs = {
	.get_modes = e17_connector_get_modes,
};

static const struct drm_connector_funcs e17_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs e17_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * ---------------------------------------------------------------------
 * Device / driver
 * ---------------------------------------------------------------------
 */

/*
 * Custom commit tail: drm_atomic_helper_commit_tail_rpm() minus
 * drm_atomic_helper_wait_for_vblanks().  This driver's plane update blits the
 * damaged region into VRAM synchronously (e17_plane_atomic_update), so the
 * pixels have already landed by commit_hw_done -- waiting for a frame boundary
 * buys nothing here and only serialises every fbcon update behind a vblank.
 * With real vblank enabled that wait is what wedged the console on hardware
 * (the waiter timed out on a cross-CPU vblank wake), so we omit it.  vblank and
 * userspace flip events still work (hrtimer source + event send in flush).
 */
static void e17_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *dev = state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	/* deliberately NO drm_atomic_helper_wait_for_vblanks() -- see above */
	drm_atomic_helper_cleanup_planes(dev, state);
}

static const struct drm_mode_config_helper_funcs e17_mode_config_helper_funcs = {
	.atomic_commit_tail = e17_commit_tail,
};

static int e17_modeset_init(struct e17_device *e17)
{
	struct drm_device *dev = &e17->dev;
	struct drm_connector *connector = &e17->connector;
	struct drm_encoder *encoder = &e17->encoder;
	struct drm_crtc *crtc = &e17->crtc;
	struct drm_plane *primary = &e17->primary_plane;
	unsigned int nformats = 0;
	int ret;

	/*
	 * Track the write-only GRMODE.  Default to the packed 8bpp value with
	 * scan-out enabled; atomic_enable overwrites it when it programs a mode.
	 */
	e17->grmode = GRMODE_INHERIT_8BPP;
	e17->active_valid = false;	/* force a full program on the first enable */

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = 640;
	dev->mode_config.max_width = 1024;
	dev->mode_config.min_height = 480;
	dev->mode_config.max_height = 768;
	dev->mode_config.preferred_depth = 8;
	dev->mode_config.funcs = &e17_mode_config_funcs;
	dev->mode_config.helper_private = &e17_mode_config_helper_funcs;
	/* Big-endian host: keep host byte order for addfb (C8 is neutral). */
	dev->mode_config.quirk_addfb_prefer_host_byte_order = true;

	e17->formats[nformats++] = DRM_FORMAT_C8;

	ret = drm_universal_plane_init(dev, primary, 1, &e17_plane_funcs,
				       e17->formats, nformats, NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(primary, &e17_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(primary);

	ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL,
					&e17_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(crtc, &e17_crtc_helper_funcs);

	/* Expose the palette to fbcon/userspace as a 256-entry gamma LUT. */
	ret = drm_mode_crtc_set_gamma_size(crtc, 256);
	if (!ret)
		drm_crtc_enable_color_mgmt(crtc, 0, false, 256);

	ret = drm_encoder_init(dev, encoder, &e17_encoder_funcs,
			       DRM_MODE_ENCODER_DAC, NULL);
	if (ret)
		return ret;
	encoder->possible_crtcs = drm_crtc_mask(crtc);

	ret = drm_connector_init(dev, connector, &e17_connector_funcs,
				 DRM_MODE_CONNECTOR_VGA);
	if (ret)
		return ret;
	drm_connector_helper_add(connector, &e17_connector_helper_funcs);
	connector->status = connector_status_connected;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	return 0;
}

DEFINE_DRM_GEM_FOPS(e17_fops);

static struct drm_driver e17_drm_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &e17_fops,
};

/*
 * Video frame interrupt (LM1882 frame pulse -> VIC LIRQ4, VIC-vectored 0x44,
 * level 5).  Counting-only: it just accounts the interrupt for /proc/interrupts.
 * DRM vblank is driven by drm's hrtimer (see e17_enable_vblank), NOT from this
 * hard-IRQ -- calling drm_crtc_handle_vblank() here wedged the console on real
 * SMP hardware.  Kept because delivery is proven and it confirms the real frame
 * rate on the board.
 */
static atomic_t e17_frame_count = ATOMIC_INIT(0);

/*
 * A runaway rate that no legitimate once-per-frame source can reach (>= this
 * many in a single jiffy, i.e. ~400k/s at HZ=100).  A stuck LM1882 frame/cursor
 * output that stays asserted would re-trigger this line -- which is a plain
 * handle_simple_irq with no HW ack/mask -- as fast as the CPU can loop, hard-
 * locking the board (only the watchdog recovers it).  Cap it defensively.
 */
#define E17_FRAME_STORM	4000

static irqreturn_t e17_frame_isr(int irq, void *dev_id)
{
	struct e17_device *e17 = dev_id;
	unsigned long now = jiffies;
	int n;

	/* Screaming-source guard: mask the line if it exceeds a sane per-jiffy rate. */
	if (now != e17->frame_jiffy) {
		e17->frame_jiffy = now;
		e17->frame_per_jiffy = 0;
	}
	if (++e17->frame_per_jiffy >= E17_FRAME_STORM) {
		disable_irq_nosync(irq);
		e17->frame_stormed = true;
		pr_warn_ratelimited(
			"e17drm: frame IRQ %d storming (>=%u/jiffy) -- masking it; "
			"the mode-set likely left the LM1882 frame output asserted\n",
			irq, E17_FRAME_STORM);
		return IRQ_HANDLED;
	}

	n = atomic_inc_return(&e17_frame_count);
	if (n == 1)
		pr_info("e17drm: frame interrupt is being delivered (IRQ %d)\n", irq);
	return IRQ_HANDLED;
}

static void e17_iounmap(void *addr)
{
	iounmap((void __iomem *)addr);
}

/*
 * Measure VRAM write bandwidth: memcpy_toio a buffer into the top half of VRAM
 * (past the 480 KB scan-out area, so the console is untouched) and report MB/s.
 * Lets vram_wt be compared on real hardware where the mapping mode actually
 * matters (it is a no-op difference under QEMU).
 */
static void e17_vram_benchmark(struct e17_device *e17)
{
	size_t off = E17_VRAM_SIZE / 2, len = E17_VRAM_SIZE / 2;
	unsigned int i, iters = 8;
	ktime_t t0, t1;
	u64 ns, bytes, mbps;
	void *src;

	src = kvmalloc(len, GFP_KERNEL);
	if (!src)
		return;
	memset(src, 0xa5, len);

	t0 = ktime_get();
	for (i = 0; i < iters; i++)
		memcpy_toio(e17->vram.vaddr_iomem + off, src, len);
	t1 = ktime_get();
	kvfree(src);

	ns = ktime_to_ns(ktime_sub(t1, t0));
	bytes = (u64)len * iters;
	mbps = ns ? div64_u64(bytes * 1000ULL, ns) : 0;	/* bytes*1e9/ns/1e6 */
	drm_info(&e17->dev, "VRAM write bench (%s): %llu KB in %llu us = %llu MB/s\n",
		 e17_vram_wt ? "write-through" : "cache-inhibited",
		 bytes >> 10, div_u64(ns, 1000), mbps);
}

static int e17_probe(struct platform_device *pdev)
{
	struct e17_device *e17;
	struct drm_device *dev;
	struct resource *vram_res;
	void __iomem *vram;
	int ret, irq;

	e17 = devm_drm_dev_alloc(&pdev->dev, &e17_drm_driver,
				 struct e17_device, dev);
	if (IS_ERR(e17))
		return PTR_ERR(e17);
	dev = &e17->dev;
	platform_set_drvdata(pdev, e17);

	/* reg 0: Bt445 RAM-DAC; reg 1: address/sync generator (MMIO). */
	e17->dac = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(e17->dac))
		return PTR_ERR(e17->dac);
	e17->ag = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(e17->ag))
		return PTR_ERR(e17->ag);

	/* reg 2: VRAM scan-out buffer. */
	vram_res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!vram_res)
		return -ENODEV;

	ret = devm_aperture_acquire_for_platform_device(pdev, vram_res->start,
							resource_size(vram_res));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "aperture acquire failed\n");

	if (!devm_request_mem_region(&pdev->dev, vram_res->start,
				     resource_size(vram_res), DRIVER_NAME))
		return -EBUSY;

	/*
	 * Default cache-inhibited (ioremap_wc, which on m68k is serialized);
	 * e17_drm.vram_wt=1 maps it write-through for faster blits.  Not devm
	 * (there is no devm_ioremap_wt), so unmap via a devm action.
	 */
	vram = e17_vram_wt ? ioremap_wt(vram_res->start, resource_size(vram_res))
			   : ioremap_wc(vram_res->start, resource_size(vram_res));
	if (!vram)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, e17_iounmap, (void __force *)vram);
	if (ret)
		return ret;
	iosys_map_set_vaddr_iomem(&e17->vram, vram);
	drm_info(dev, "VRAM %pa mapped %s\n", &vram_res->start,
		 e17_vram_wt ? "write-through" : "cache-inhibited");

	if (e17_vram_bench)
		e17_vram_benchmark(e17);

	/* Probe the Bt445 by its ID register (must read 0x3a). */
	{
		u8 id = bt445_read_id(e17);

		if (id != BT_ID_VALUE)
			drm_warn(dev, "Bt445 ID mismatch (read 0x%02x, expected 0x%02x)\n",
				 id, BT_ID_VALUE);
	}

	ret = e17_modeset_init(e17);
	if (ret)
		return ret;

	/*
	 * Vblank (opt-in, e17_drm.vblank=1) is driven by drm's hrtimer (see
	 * e17_enable_vblank) and does not depend on the frame IRQ; init it before
	 * drm_dev_register.  With it off, drm_dev_has_vblank() stays false so the
	 * fbdev damage worker never waits on vblank -- the stable default.
	 */
	if (e17_vblank) {
		ret = drm_vblank_init(dev, 1);
		if (ret)
			return ret;
	}

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_client_setup(dev, NULL);	/* fbdev at preferred_depth (C8/8bpp) */

	/*
	 * Video frame interrupt: route the LM1882 frame pulse to VIC LIRQ4 and
	 * request it as a counting-only ISR (visible in /proc/interrupts, confirms
	 * the real frame rate).  It does NOT drive vblank -- see e17_frame_isr.
	 * $FEC5.4001 FR bit = 0xfc, RMON's default; write-only, never RMW.  NB: the
	 * HW manual Table 38 claims FR=0 routes to LIRQ4 and FR=1 disables, but the
	 * board is the opposite (RMON leaves 0xfc / FR set and the frame IRQ fires,
	 * confirmed on hardware) -- keep 0xfc, do not "fix" it to the manual.
	 *
	 * platform_get_irq_optional() maps LIRQ4, which masks LICR4 (irqchip
	 * domain_map default), so when frame_irq is off we simply do not route or
	 * request it: the line stays masked and no frame interrupt is delivered.
	 */
	irq = platform_get_irq_optional(pdev, 0);
	if (!e17_frame_irq) {
		drm_info(dev, "frame interrupt disabled (e17_drm.frame_irq=0)\n");
	} else if (irq > 0) {
		void __iomem *irqroute = devm_platform_ioremap_resource(pdev, 3);

		if (!IS_ERR(irqroute))
			writeb(0xfc, irqroute + 1);	/* FR -> LIRQ4 */
		if (devm_request_irq(&pdev->dev, irq, e17_frame_isr, 0,
				     "e17-frame", e17))
			drm_warn(dev, "cannot request frame IRQ %d\n", irq);
		else
			drm_info(dev, "frame interrupt on IRQ %d (counting)\n", irq);
	}
	return 0;
}

static void e17_remove(struct platform_device *pdev)
{
	struct e17_device *e17 = platform_get_drvdata(pdev);

	drm_dev_unplug(&e17->dev);
	drm_atomic_helper_shutdown(&e17->dev);
}

static void e17_shutdown(struct platform_device *pdev)
{
	struct e17_device *e17 = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&e17->dev);
}

static int e17_pm_suspend(struct device *dev)
{
	struct e17_device *e17 = dev_get_drvdata(dev);

	/* Power-off loses the hardware timing; force a full reprogram on resume. */
	e17->active_valid = false;
	return drm_mode_config_helper_suspend(&e17->dev);
}

static int e17_pm_resume(struct device *dev)
{
	struct e17_device *e17 = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(&e17->dev);
}

static DEFINE_SIMPLE_DEV_PM_OPS(e17_pm_ops, e17_pm_suspend, e17_pm_resume);

static const struct of_device_id e17_of_match[] = {
	{ .compatible = "eltec,e17-video" },
	{ }
};
MODULE_DEVICE_TABLE(of, e17_of_match);

static struct platform_driver e17_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = e17_of_match,
		.pm = pm_sleep_ptr(&e17_pm_ops),
	},
	.probe = e17_probe,
	.remove = e17_remove,
	.shutdown = e17_shutdown,
};

module_platform_driver(e17_platform_driver);

MODULE_AUTHOR("ELTEC Eurocom E17 bring-up");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
