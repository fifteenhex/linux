// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * macfb.c: Generic framebuffer for Macs whose colourmaps/modes we
 * don't know how to set.
 *
 * (c) 1999 David Huggins-Daines <dhd@debian.org>
 *
 * Primarily based on vesafb.c, by Gerd Knorr
 * (c) 1998 Gerd Knorr <kraxel@cs.tu-berlin.de>
 *
 * Also uses information and code from:
 *
 * The original macfb.c from Linux/mac68k 2.0, by Alan Cox, Juergen
 * Mellinger, Mikael Forselius, Michael Schmitz, and others.
 *
 * valkyriefb.c, by Martin Costabel, Kevin Schoedel, Barry Nathan, Dan
 * Jacobowitz, Paul Mackerras, Fabio Riccardi, and Geert Uytterhoeven.
 *
 * The VideoToolbox "Bugs" web page at
 * http://rajsky.psych.nyu.edu/Tips/VideoBugs.html
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/nubus.h>
#include <linux/init.h>
#include <linux/fb.h>

#include <asm/setup.h>
#include <asm/macintosh.h>
#include <asm/io.h>

/* Common DAC base address for the LC, RBV, Valkyrie, and IIvx */
#define DAC_BASE 0x50f24000

/* Some addresses for the DAFB */
#define DAFB_BASE 0xf9800200

/* Address for the built-in Civic framebuffer in Quadra AVs */
#define CIVIC_BASE 0x50f30800

/* GSC (Gray Scale Controller) base address */
#define GSC_BASE 0x50F20000

/* CSC (Color Screen Controller) base address */
#define CSC_BASE 0x50F20000

static int (*macfb_setpalette)(unsigned int regno, unsigned int red,
			       unsigned int green, unsigned int blue,
			       struct fb_info *info);

static struct {
	unsigned char addr;
	unsigned char lut;
} __iomem *v8_brazil_cmap_regs;

static struct {
	unsigned char addr;
	char pad1[3]; /* word aligned */
	unsigned char lut;
	char pad2[3]; /* word aligned */
	unsigned char cntl; /* a guess as to purpose */
} __iomem *rbv_cmap_regs;

static struct {
	unsigned long reset;
	unsigned long pad1[3];
	unsigned char pad2[3];
	unsigned char lut;
} __iomem *dafb_cmap_regs;

static struct {
	unsigned char addr;	/* OFFSET: 0x00 */
	unsigned char pad1[15];
	unsigned char lut;	/* OFFSET: 0x10 */
	unsigned char pad2[15];
	unsigned char status;	/* OFFSET: 0x20 */
	unsigned char pad3[7];
	unsigned long vbl_addr;	/* OFFSET: 0x28 */
	unsigned int  status2;	/* OFFSET: 0x2C */
} __iomem *civic_cmap_regs;

static struct {
	char pad1[0x40];
	unsigned char clut_waddr;	/* 0x40 */
	char pad2;
	unsigned char clut_data;	/* 0x42 */
	char pad3[0x3];
	unsigned char clut_raddr;	/* 0x46 */
} __iomem *csc_cmap_regs;

/* The registers in these structs are in NuBus slot space */
struct mdc_cmap_regs {
	char pad1[0x200200];
	unsigned char addr;
	char pad2[6];
	unsigned char lut;
};

struct toby_cmap_regs {
	char pad1[0x90018];
	unsigned char lut; /* TFBClutWDataReg, offset 0x90018 */
	char pad2[3];
	unsigned char addr; /* TFBClutAddrReg, offset 0x9001C */
};

struct jet_cmap_regs {
	char pad1[0xe0e000];
	unsigned char addr;
	unsigned char lut;
};

#define PIXEL_TO_MM(a)	(((a)*10)/28)	/* width in mm at 72 dpi */

static struct fb_var_screeninfo macfb_defined = {
	.activate	= FB_ACTIVATE_NOW,
	.right_margin	= 32,
	.upper_margin	= 16,
	.lower_margin	= 4,
	.vsync_len	= 4,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo macfb_fix = {
	.type	= FB_TYPE_PACKED_PIXELS,
	.accel	= FB_ACCEL_NONE,
};

static void *slot_addr;
static struct fb_info fb_info;
static u32 pseudo_palette[16];
static int vidtest;

/*
 * Unlike the Valkyrie, the DAFB cannot set individual colormap
 * registers.  Therefore, we do what the MacOS driver does (no
 * kidding!) and simply set them one by one until we hit the one we
 * want.
 */
static int dafb_setpalette(unsigned int regno, unsigned int red,
			   unsigned int green, unsigned int blue,
			   struct fb_info *info)
{
	static int lastreg = -2;
	unsigned long flags;

	local_irq_save(flags);

	/*
	 * fbdev will set an entire colourmap, but X won't.  Hopefully
	 * this should accommodate both of them
	 */
	if (regno != lastreg + 1) {
		int i;

		/* Stab in the dark trying to reset the CLUT pointer */
		nubus_writel(0, &dafb_cmap_regs->reset);
		nop();

		/* Loop until we get to the register we want */
		for (i = 0; i < regno; i++) {
			nubus_writeb(info->cmap.red[i] >> 8,
				     &dafb_cmap_regs->lut);
			nop();
			nubus_writeb(info->cmap.green[i] >> 8,
				     &dafb_cmap_regs->lut);
			nop();
			nubus_writeb(info->cmap.blue[i] >> 8,
				     &dafb_cmap_regs->lut);
			nop();
		}
	}

	nubus_writeb(red, &dafb_cmap_regs->lut);
	nop();
	nubus_writeb(green, &dafb_cmap_regs->lut);
	nop();
	nubus_writeb(blue, &dafb_cmap_regs->lut);

	local_irq_restore(flags);
	lastreg = regno;
	return 0;
}

/* V8 and Brazil seem to use the same DAC.  Sonora does as well. */
static int v8_brazil_setpalette(unsigned int regno, unsigned int red,
				unsigned int green, unsigned int blue,
				struct fb_info *info)
{
	unsigned int bpp = info->var.bits_per_pixel;
	unsigned long flags;

	local_irq_save(flags);

	/* On these chips, the CLUT register numbers are spread out
	 * across the register space.  Thus:
	 * In 8bpp, all regnos are valid.
	 * In 4bpp, the regnos are 0x0f, 0x1f, 0x2f, etc, etc
	 * In 2bpp, the regnos are 0x3f, 0x7f, 0xbf, 0xff
	 */
	regno = (regno << (8 - bpp)) | (0xFF >> bpp);
	nubus_writeb(regno, &v8_brazil_cmap_regs->addr);
	nop();

	/* send one color channel at a time */
	nubus_writeb(red, &v8_brazil_cmap_regs->lut);
	nop();
	nubus_writeb(green, &v8_brazil_cmap_regs->lut);
	nop();
	nubus_writeb(blue, &v8_brazil_cmap_regs->lut);

	local_irq_restore(flags);
	return 0;
}

/* RAM-Based Video */
static int rbv_setpalette(unsigned int regno, unsigned int red,
			  unsigned int green, unsigned int blue,
			  struct fb_info *info)
{
	unsigned long flags;

	local_irq_save(flags);

	/* From the VideoToolbox driver.  Seems to be saying that
	 * regno #254 and #255 are the important ones for 1-bit color,
	 * regno #252-255 are the important ones for 2-bit color, etc.
	 */
	regno += 256 - (1 << info->var.bits_per_pixel);

	/* reset clut? (VideoToolbox sez "not necessary") */
	nubus_writeb(0xFF, &rbv_cmap_regs->cntl);
	nop();

	/* tell clut which address to use. */
	nubus_writeb(regno, &rbv_cmap_regs->addr);
	nop();

	/* send one color channel at a time. */
	nubus_writeb(red, &rbv_cmap_regs->lut);
	nop();
	nubus_writeb(green, &rbv_cmap_regs->lut);
	nop();
	nubus_writeb(blue, &rbv_cmap_regs->lut);

	local_irq_restore(flags);
	return 0;
}

/* Macintosh Display Card (8*24) */
static int mdc_setpalette(unsigned int regno, unsigned int red,
			  unsigned int green, unsigned int blue,
			  struct fb_info *info)
{
	struct mdc_cmap_regs *cmap_regs = slot_addr;
	unsigned long flags;

	local_irq_save(flags);

	/* the nop's are there to order writes. */
	nubus_writeb(regno, &cmap_regs->addr);
	nop();
	nubus_writeb(red, &cmap_regs->lut);
	nop();
	nubus_writeb(green, &cmap_regs->lut);
	nop();
	nubus_writeb(blue, &cmap_regs->lut);

	local_irq_restore(flags);
	return 0;
}

/* Toby frame buffer */
static int toby_setpalette(unsigned int regno, unsigned int red,
			   unsigned int green, unsigned int blue,
			   struct fb_info *info)
{
	struct toby_cmap_regs *cmap_regs = slot_addr;
	unsigned int bpp = info->var.bits_per_pixel;
	unsigned long flags;

	red = ~red;
	green = ~green;
	blue = ~blue;
	regno = (regno << (8 - bpp)) | (0xFF >> bpp);

	local_irq_save(flags);

	nubus_writeb(regno, &cmap_regs->addr);
	nop();
	nubus_writeb(red, &cmap_regs->lut);
	nop();
	nubus_writeb(green, &cmap_regs->lut);
	nop();
	nubus_writeb(blue, &cmap_regs->lut);

	local_irq_restore(flags);
	return 0;
}

/* Jet frame buffer */
static int jet_setpalette(unsigned int regno, unsigned int red,
			  unsigned int green, unsigned int blue,
			  struct fb_info *info)
{
	struct jet_cmap_regs *cmap_regs = slot_addr;
	unsigned long flags;

	local_irq_save(flags);

	nubus_writeb(regno, &cmap_regs->addr);
	nop();
	nubus_writeb(red, &cmap_regs->lut);
	nop();
	nubus_writeb(green, &cmap_regs->lut);
	nop();
	nubus_writeb(blue, &cmap_regs->lut);

	local_irq_restore(flags);
	return 0;
}

/*
 * Civic framebuffer -- Quadra AV built-in video.  A chip
 * called Sebastian holds the actual color palettes, and
 * apparently, there are two different banks of 512K RAM
 * which can act as separate framebuffers for doing video
 * input and viewing the screen at the same time!  The 840AV
 * Can add another 1MB RAM to give the two framebuffers
 * 1MB RAM apiece.
 */
static int civic_setpalette(unsigned int regno, unsigned int red,
			    unsigned int green, unsigned int blue,
			    struct fb_info *info)
{
	unsigned long flags;
	int clut_status;

	local_irq_save(flags);

	/* Set the register address */
	nubus_writeb(regno, &civic_cmap_regs->addr);
	nop();

	/*
	 * Grab a status word and do some checking;
	 * Then finally write the clut!
	 */
	clut_status =  nubus_readb(&civic_cmap_regs->status2);

	if ((clut_status & 0x0008) == 0)
	{
#if 0
		if ((clut_status & 0x000D) != 0)
		{
			nubus_writeb(0x00, &civic_cmap_regs->lut);
			nop();
			nubus_writeb(0x00, &civic_cmap_regs->lut);
			nop();
		}
#endif

		nubus_writeb(red, &civic_cmap_regs->lut);
		nop();
		nubus_writeb(green, &civic_cmap_regs->lut);
		nop();
		nubus_writeb(blue, &civic_cmap_regs->lut);
		nop();
		nubus_writeb(0x00, &civic_cmap_regs->lut);
	}
	else
	{
		unsigned char junk;

		junk = nubus_readb(&civic_cmap_regs->lut);
		nop();
		junk = nubus_readb(&civic_cmap_regs->lut);
		nop();
		junk = nubus_readb(&civic_cmap_regs->lut);
		nop();
		junk = nubus_readb(&civic_cmap_regs->lut);
		nop();

		if ((clut_status & 0x000D) != 0)
		{
			nubus_writeb(0x00, &civic_cmap_regs->lut);
			nop();
			nubus_writeb(0x00, &civic_cmap_regs->lut);
			nop();
		}

		nubus_writeb(red, &civic_cmap_regs->lut);
		nop();
		nubus_writeb(green, &civic_cmap_regs->lut);
		nop();
		nubus_writeb(blue, &civic_cmap_regs->lut);
		nop();
		nubus_writeb(junk, &civic_cmap_regs->lut);
	}

	local_irq_restore(flags);
	return 0;
}

/*
 * The CSC is the framebuffer on the PowerBook 190 series
 * (and the 5300 too, but that's a PowerMac). This function
 * brought to you in part by the ECSC driver for MkLinux.
 */
static int csc_setpalette(unsigned int regno, unsigned int red,
			  unsigned int green, unsigned int blue,
			  struct fb_info *info)
{
	unsigned long flags;

	local_irq_save(flags);

	udelay(1); /* mklinux on PB 5300 waits for 260 ns */
	nubus_writeb(regno, &csc_cmap_regs->clut_waddr);
	nubus_writeb(red, &csc_cmap_regs->clut_data);
	nubus_writeb(green, &csc_cmap_regs->clut_data);
	nubus_writeb(blue, &csc_cmap_regs->clut_data);

	local_irq_restore(flags);
	return 0;
}

static int macfb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp,
			   struct fb_info *fb_info)
{
	/*
	 * Set a single color register. The values supplied are
	 * already rounded down to the hardware's capabilities
	 * (according to the entries in the `var' structure).
	 * Return non-zero for invalid regno.
	 */

	if (regno >= fb_info->cmap.len)
		return 1;

	if (fb_info->var.bits_per_pixel <= 8) {
		switch (fb_info->var.bits_per_pixel) {
		case 1:
			/* We shouldn't get here */
			break;
		case 2:
		case 4:
		case 8:
			if (macfb_setpalette)
				macfb_setpalette(regno, red >> 8, green >> 8,
						 blue >> 8, fb_info);
			else
				return 1;
			break;
		}
	} else if (regno < 16) {
		switch (fb_info->var.bits_per_pixel) {
		case 16:
			if (fb_info->var.red.offset == 10) {
				/* 1:5:5:5 */
				((u32*) (fb_info->pseudo_palette))[regno] =
					((red   & 0xf800) >>  1) |
					((green & 0xf800) >>  6) |
					((blue  & 0xf800) >> 11) |
					((transp != 0) << 15);
			} else {
				/* 0:5:6:5 */
				((u32*) (fb_info->pseudo_palette))[regno] =
					((red   & 0xf800) >>  0) |
					((green & 0xfc00) >>  5) |
					((blue  & 0xf800) >> 11);
			}
			break;
		/*
		 * 24-bit colour almost doesn't exist on 68k Macs --
		 * https://support.apple.com/kb/TA28634 (Old Article: 10992)
		 */
		case 24:
		case 32:
			red   >>= 8;
			green >>= 8;
			blue  >>= 8;
			((u32 *)(fb_info->pseudo_palette))[regno] =
				(red   << fb_info->var.red.offset) |
				(green << fb_info->var.green.offset) |
				(blue  << fb_info->var.blue.offset);
			break;
		}
	}

	return 0;
}

static const struct fb_ops macfb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_setcolreg	= macfb_setcolreg,
};

/*
 * Radius PrecisionColor 24Xp acceleration.
 *
 * The card exposes a VRAM block-transfer engine through a second aperture
 * one slot-quadrant above the framebuffer (slot offset 0x400000; aperture
 * offset X operates on VRAM offset X).  The engine is a 128-byte data ring
 * between VRAM and VRAM with an input cursor advanced by aperture reads and
 * an output cursor advanced by aperture writes (both mod 128):
 *
 *   - A read appends the naturally-aligned power-of-two span selected by the
 *     access's low address bits [span = 2 * 2^(trailing ones of (addr | 1)),
 *     base = addr & ~(span-1)] from VRAM[base] into the ring.
 *   - A 32-bit write of 0x40000000 | N stores N ring bytes (from the output
 *     cursor, wrapping mod 128) to VRAM[offset].
 *   - A 32-bit write of 8 loads the 8 bytes at VRAM[offset] tiled across the
 *     whole ring and resets both cursors -- the pattern-replicate used by
 *     fills.
 *
 * We drive fills with the replicate + store commands and copies with span
 * reads feeding store commands (the ring is a FIFO, so src == dst mod 4 and
 * 4-byte alignment are sufficient; anything else falls back to software).
 * Text (imageblit) and the XOR text cursor stay on the cfb_* CPU paths.
 */
#define RADIUS_BLIT_OFF		0x400000	/* engine aperture, slot-relative */
#define RADIUS_BLIT_LEN		0x400000
#define RADIUS_SCRATCH_OFF	0x3FF800	/* reserved top of VRAM: 8B pattern */

/* DrHW of the 24Xp video sResource: 0x3EB in ROM v1.32, 0x406 in ROM v2.0 */
#define NUBUS_DRHW_RADIUS_24XP		0x0406
#define NUBUS_DRHW_RADIUS_24XP_V1	0x03EB

static u8 __iomem *radius_blit;		/* the 0x400000 engine aperture */
static u8 __iomem *radius_scratch;	/* 8-byte fill pattern staging (VRAM) */

/* Append n bytes (multiple of 4) starting at the 4-aligned VRAM offset base
 * into the ring, using the largest naturally-aligned span at each step. */
static void radius_ring_append(u32 base, u32 n)
{
	u32 p = 0;

	while (p < n) {
		u32 addr = base + p;
		u32 rem = n - p;
		u32 span = 4, cand;

		for (cand = 8; cand <= 64; cand <<= 1)
			if (!(addr & (cand - 1)) && cand <= rem)
				span = cand;
		/* a byte read at base + span/2 - 2 selects exactly this span */
		nubus_readb(radius_blit + addr + (span >> 1) - 2);
		p += span;
	}
}

/* Store n ring bytes to VRAM offset off (engine command word). */
static inline void radius_ring_store(u32 off, u32 n)
{
	nubus_writel(0x40000000 | n, radius_blit + off);
}

static void radius_fillrect(struct fb_info *info, const struct fb_fillrect *r)
{
	u32 bpp = info->var.bits_per_pixel >> 3;
	u32 ll = info->fix.line_length;
	u32 wbytes, y, fg, i;

	/* only solid ROP_COPY fills of the truecolour modes we accelerate */
	if (r->rop != ROP_COPY || (bpp != 2 && bpp != 4)) {
		cfb_fillrect(info, r);
		return;
	}
	if (info->fix.visual == FB_VISUAL_TRUECOLOR ||
	    info->fix.visual == FB_VISUAL_DIRECTCOLOR)
		fg = ((u32 *)info->pseudo_palette)[r->color];
	else
		fg = r->color;

	/* stage an 8-byte pattern in VRAM, then tile it across the ring */
	if (bpp == 2)
		for (i = 0; i < 4; i++)
			nubus_writew(fg, radius_scratch + 2 * i);
	else
		for (i = 0; i < 2; i++)
			nubus_writel(fg, radius_scratch + 4 * i);
	nubus_writel(8, radius_blit + RADIUS_SCRATCH_OFF);	/* replicate + reset */

	wbytes = r->width * bpp;
	for (y = 0; y < r->height; y++)
		radius_ring_store((r->dy + y) * ll + r->dx * bpp, wbytes);
}

static void radius_copyarea(struct fb_info *info, const struct fb_copyarea *a)
{
	u32 bpp = info->var.bits_per_pixel >> 3;
	u32 ll = info->fix.line_length;
	u32 wbytes = a->width * bpp;
	u32 i;
	long row, rstep, rstart;

	/* The ring FIFO needs src == dst (mod 4) and 4-byte-aligned runs. */
	if ((bpp != 2 && bpp != 4) || (ll & 3) || (wbytes & 3) ||
	    ((a->sx * bpp) & 3) || ((a->dx * bpp) & 3) ||
	    /* horizontally overlapping in-place moves need right-to-left */
	    (a->sy == a->dy && a->dx > a->sx && a->dx < a->sx + a->width)) {
		cfb_copyarea(info, a);
		return;
	}

	/* walk rows away from the overlap so a source row is read before the
	 * transfer overwrites it */
	if (a->dy > a->sy) {
		rstart = a->height - 1;
		rstep = -1;
	} else {
		rstart = 0;
		rstep = 1;
	}

	for (i = 0, row = rstart; i < a->height; i++, row += rstep) {
		u32 s = (a->sy + row) * ll + a->sx * bpp;
		u32 d = (a->dy + row) * ll + a->dx * bpp;
		u32 off = 0;

		while (off < wbytes) {
			u32 blk = min_t(u32, 128, wbytes - off);

			radius_ring_append(s + off, blk);
			radius_ring_store(d + off, blk);
			off += blk;
		}
	}
}

static const struct fb_ops radius_ops = {
	.owner		= THIS_MODULE,
	__FB_DEFAULT_IOMEM_OPS_RDWR,
	.fb_setcolreg	= macfb_setcolreg,
	.fb_fillrect	= radius_fillrect,
	.fb_copyarea	= radius_copyarea,
	.fb_imageblit	= cfb_imageblit,
	__FB_DEFAULT_IOMEM_OPS_MMAP,
};

static void __init macfb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;

		if (!strcmp(this_opt, "inverse"))
			fb_invert_cmaps();
		else
			if (!strcmp(this_opt, "vidtest"))
				vidtest = 1; /* enable experimental CLUT code */
	}
}

static void __init iounmap_macfb(void)
{
	if (dafb_cmap_regs)
		iounmap(dafb_cmap_regs);
	if (v8_brazil_cmap_regs)
		iounmap(v8_brazil_cmap_regs);
	if (rbv_cmap_regs)
		iounmap(rbv_cmap_regs);
	if (civic_cmap_regs)
		iounmap(civic_cmap_regs);
	if (csc_cmap_regs)
		iounmap(csc_cmap_regs);
	if (radius_blit)
		iounmap(radius_blit);
	if (radius_scratch)
		iounmap(radius_scratch);
}

static int __init macfb_init(void)
{
	int video_cmap_len, video_is_nubus = 0;
	struct nubus_rsrc *ndev = NULL;
	char *option = NULL;
	int err;

	if (fb_get_options("macfb", &option))
		return -ENODEV;
	macfb_setup(option);

	if (!MACH_IS_MAC)
		return -ENODEV;

	if (mac_bi_data.id == MAC_MODEL_Q630 ||
	    mac_bi_data.id == MAC_MODEL_P588)
		return -ENODEV; /* See valkyriefb.c */

	macfb_defined.xres = mac_bi_data.dimensions & 0xFFFF;
	macfb_defined.yres = mac_bi_data.dimensions >> 16;
	macfb_defined.bits_per_pixel = mac_bi_data.videodepth;

	macfb_fix.line_length = mac_bi_data.videorow;
	macfb_fix.smem_len    = macfb_fix.line_length * macfb_defined.yres;
	/* Note: physical address (since 2.1.127) */
	macfb_fix.smem_start  = mac_bi_data.videoaddr;

	/*
	 * This is actually redundant with the initial mappings.
	 * However, there are some non-obvious aspects to the way
	 * those mappings are set up, so this is in fact the safest
	 * way to ensure that this driver will work on every possible Mac
	 */
	fb_info.screen_base = ioremap(mac_bi_data.videoaddr,
				      macfb_fix.smem_len);
	if (!fb_info.screen_base)
		return -ENODEV;

	pr_info("macfb: framebuffer at 0x%08lx, mapped to 0x%p, size %dk\n",
	        macfb_fix.smem_start, fb_info.screen_base,
	        macfb_fix.smem_len / 1024);
	pr_info("macfb: mode is %dx%dx%d, linelength=%d\n",
	        macfb_defined.xres, macfb_defined.yres,
	        macfb_defined.bits_per_pixel, macfb_fix.line_length);

	/* Fill in the available video resolution */
	macfb_defined.xres_virtual = macfb_defined.xres;
	macfb_defined.yres_virtual = macfb_defined.yres;
	macfb_defined.height       = PIXEL_TO_MM(macfb_defined.yres);
	macfb_defined.width        = PIXEL_TO_MM(macfb_defined.xres);

	/* Some dummy values for timing to make fbset happy */
	macfb_defined.pixclock     = 10000000 / macfb_defined.xres *
				     1000 / macfb_defined.yres;
	macfb_defined.left_margin  = (macfb_defined.xres / 8) & 0xf8;
	macfb_defined.hsync_len    = (macfb_defined.xres / 8) & 0xf8;

	switch (macfb_defined.bits_per_pixel) {
	case 1:
		macfb_defined.red.length = macfb_defined.bits_per_pixel;
		macfb_defined.green.length = macfb_defined.bits_per_pixel;
		macfb_defined.blue.length = macfb_defined.bits_per_pixel;
		video_cmap_len = 2;
		macfb_fix.visual = FB_VISUAL_MONO01;
		break;
	case 2:
	case 4:
	case 8:
		macfb_defined.red.length = macfb_defined.bits_per_pixel;
		macfb_defined.green.length = macfb_defined.bits_per_pixel;
		macfb_defined.blue.length = macfb_defined.bits_per_pixel;
		video_cmap_len = 1 << macfb_defined.bits_per_pixel;
		macfb_fix.visual = FB_VISUAL_PSEUDOCOLOR;
		break;
	case 16:
		macfb_defined.transp.offset = 15;
		macfb_defined.transp.length = 1;
		macfb_defined.red.offset = 10;
		macfb_defined.red.length = 5;
		macfb_defined.green.offset = 5;
		macfb_defined.green.length = 5;
		macfb_defined.blue.offset = 0;
		macfb_defined.blue.length = 5;
		video_cmap_len = 16;
		/*
		 * Should actually be FB_VISUAL_DIRECTCOLOR, but this
		 * works too
		 */
		macfb_fix.visual = FB_VISUAL_TRUECOLOR;
		break;
	case 24:
	case 32:
		macfb_defined.red.offset = 16;
		macfb_defined.red.length = 8;
		macfb_defined.green.offset = 8;
		macfb_defined.green.length = 8;
		macfb_defined.blue.offset = 0;
		macfb_defined.blue.length = 8;
		video_cmap_len = 16;
		macfb_fix.visual = FB_VISUAL_TRUECOLOR;
		break;
	default:
		pr_err("macfb: unknown or unsupported bit depth: %d\n",
		       macfb_defined.bits_per_pixel);
		err = -EINVAL;
		goto fail_unmap;
	}

	/*
	 * We take a wild guess that if the video physical address is
	 * in nubus slot space, that the nubus card is driving video.
	 * Penguin really ought to tell us whether we are using internal
	 * video or not.
	 * Hopefully we only find one of them.  Otherwise our NuBus
	 * code is really broken :-)
	 */

	for_each_func_rsrc(ndev) {
		unsigned long base = ndev->board->slot_addr;

		if (mac_bi_data.videoaddr < base ||
		    mac_bi_data.videoaddr - base > 0xFFFFFF)
			continue;

		if (ndev->category != NUBUS_CAT_DISPLAY ||
		    ndev->type != NUBUS_TYPE_VIDEO)
			continue;

		video_is_nubus = 1;
		slot_addr = (unsigned char *)base;

		switch(ndev->dr_hw) {
		case NUBUS_DRHW_APPLE_MDC:
			strscpy(macfb_fix.id, "Mac Disp. Card");
			macfb_setpalette = mdc_setpalette;
			break;
		case NUBUS_DRHW_APPLE_TFB:
			strscpy(macfb_fix.id, "Toby");
			macfb_setpalette = toby_setpalette;
			break;
		case NUBUS_DRHW_APPLE_JET:
			strscpy(macfb_fix.id, "Jet");
			macfb_setpalette = jet_setpalette;
			break;
		case NUBUS_DRHW_RDIUS_PC24XP:
		case NUBUS_DRHW_RDIUS_PC24XP_V1:
			strscpy(macfb_fix.id, "Radius 24Xp");
			/*
			 * Map the block-transfer engine aperture and the VRAM
			 * pattern-staging area; if either fails we simply fall
			 * back to the generic (software) framebuffer path.
			 */
			radius_blit = ioremap(base + RADIUS_BLIT_OFF,
					      RADIUS_BLIT_LEN);
			radius_scratch = ioremap(base + RADIUS_SCRATCH_OFF, 8);
			if (!radius_blit || !radius_scratch) {
				if (radius_blit)
					iounmap(radius_blit);
				if (radius_scratch)
					iounmap(radius_scratch);
				radius_blit = NULL;
				radius_scratch = NULL;
			}
			break;
		default:
			strscpy(macfb_fix.id, "Generic NuBus");
			break;
		}
	}

	/* If it's not a NuBus card, it must be internal video */
	if (!video_is_nubus)
		switch (mac_bi_data.id) {
		/*
		 * DAFB Quadras
		 * Note: these first four have the v7 DAFB, which is
		 * known to be rather unlike the ones used in the
		 * other models
		 */
		case MAC_MODEL_P475:
		case MAC_MODEL_P475F:
		case MAC_MODEL_P575:
		case MAC_MODEL_Q605:

		case MAC_MODEL_Q800:
		case MAC_MODEL_Q650:
		case MAC_MODEL_Q610:
		case MAC_MODEL_C650:
		case MAC_MODEL_C610:
		case MAC_MODEL_Q700:
		case MAC_MODEL_Q900:
		case MAC_MODEL_Q950:
			strscpy(macfb_fix.id, "DAFB");
			macfb_setpalette = dafb_setpalette;
			dafb_cmap_regs = ioremap(DAFB_BASE, 0x1000);
			break;

		/*
		 * LC II uses the V8 framebuffer
		 */
		case MAC_MODEL_LCII:
			strscpy(macfb_fix.id, "V8");
			macfb_setpalette = v8_brazil_setpalette;
			v8_brazil_cmap_regs = ioremap(DAC_BASE, 0x1000);
			break;

		/*
		 * IIvi, IIvx use the "Brazil" framebuffer (which is
		 * very much like the V8, it seems, and probably uses
		 * the same DAC)
		 */
		case MAC_MODEL_IIVI:
		case MAC_MODEL_IIVX:
		case MAC_MODEL_P600:
			strscpy(macfb_fix.id, "Brazil");
			macfb_setpalette = v8_brazil_setpalette;
			v8_brazil_cmap_regs = ioremap(DAC_BASE, 0x1000);
			break;

		/*
		 * LC III (and friends) use the Sonora framebuffer
		 * Incidentally this is also used in the non-AV models
		 * of the x100 PowerMacs
		 * These do in fact seem to use the same DAC interface
		 * as the LC II.
		 */
		case MAC_MODEL_LCIII:
		case MAC_MODEL_P520:
		case MAC_MODEL_P550:
		case MAC_MODEL_P460:
			strscpy(macfb_fix.id, "Sonora");
			macfb_setpalette = v8_brazil_setpalette;
			v8_brazil_cmap_regs = ioremap(DAC_BASE, 0x1000);
			break;

		/*
		 * IIci and IIsi use the infamous RBV chip
		 * (the IIsi is just a rebadged and crippled
		 * IIci in a different case, BTW)
		 */
		case MAC_MODEL_IICI:
		case MAC_MODEL_IISI:
			strscpy(macfb_fix.id, "RBV");
			macfb_setpalette = rbv_setpalette;
			rbv_cmap_regs = ioremap(DAC_BASE, 0x1000);
			break;

		/*
		 * AVs use the Civic framebuffer
		 */
		case MAC_MODEL_Q840:
		case MAC_MODEL_C660:
			strscpy(macfb_fix.id, "Civic");
			macfb_setpalette = civic_setpalette;
			civic_cmap_regs = ioremap(CIVIC_BASE, 0x1000);
			break;


		/*
		 * Assorted weirdos
		 * We think this may be like the LC II
		 */
		case MAC_MODEL_LC:
			strscpy(macfb_fix.id, "LC");
			if (vidtest) {
				macfb_setpalette = v8_brazil_setpalette;
				v8_brazil_cmap_regs =
					ioremap(DAC_BASE, 0x1000);
			}
			break;

		/*
		 * We think this may be like the LC II
		 */
		case MAC_MODEL_CCL:
			strscpy(macfb_fix.id, "Color Classic");
			if (vidtest) {
				macfb_setpalette = v8_brazil_setpalette;
				v8_brazil_cmap_regs =
					ioremap(DAC_BASE, 0x1000);
			}
			break;

		/*
		 * And we *do* mean "weirdos"
		 */
		case MAC_MODEL_TV:
			strscpy(macfb_fix.id, "Mac TV");
			break;

		/*
		 * These don't have colour, so no need to worry
		 */
		case MAC_MODEL_SE30:
		case MAC_MODEL_CLII:
			strscpy(macfb_fix.id, "Monochrome");
			break;

		/*
		 * Powerbooks are particularly difficult.  Many of
		 * them have separate framebuffers for external and
		 * internal video, which is admittedly pretty cool,
		 * but will be a bit of a headache to support here.
		 * Also, many of them are grayscale, and we don't
		 * really support that.
		 */

		/*
		 * Slot 0 ROM says TIM. No external video. B&W.
		 */
		case MAC_MODEL_PB140:
		case MAC_MODEL_PB145:
		case MAC_MODEL_PB170:
			strscpy(macfb_fix.id, "DDC");
			break;

		/*
		 * Internal is GSC, External (if present) is ViSC
		 */
		case MAC_MODEL_PB150:	/* no external video */
		case MAC_MODEL_PB160:
		case MAC_MODEL_PB165:
		case MAC_MODEL_PB180:
		case MAC_MODEL_PB210:
		case MAC_MODEL_PB230:
			strscpy(macfb_fix.id, "GSC");
			break;

		/*
		 * Internal is TIM, External is ViSC
		 */
		case MAC_MODEL_PB165C:
		case MAC_MODEL_PB180C:
			strscpy(macfb_fix.id, "TIM");
			break;

		/*
		 * Internal is CSC, External is Keystone+Ariel.
		 */
		case MAC_MODEL_PB190:	/* external video is optional */
		case MAC_MODEL_PB520:
		case MAC_MODEL_PB250:
		case MAC_MODEL_PB270C:
		case MAC_MODEL_PB280:
		case MAC_MODEL_PB280C:
			strscpy(macfb_fix.id, "CSC");
			macfb_setpalette = csc_setpalette;
			csc_cmap_regs = ioremap(CSC_BASE, 0x1000);
			break;

		default:
			strscpy(macfb_fix.id, "Unknown");
			break;
		}

	/* accelerate through the engine aperture when it mapped, else software */
	if (radius_blit) {
		fb_info.fbops = &radius_ops;
		/*
		 * Advertise the fill/copy engine.  fbcon only scrolls via
		 * fb_copyarea (SCROLL_MOVE) when HWACCEL_COPYAREA is set; we
		 * leave IMAGEBLIT off (glyphs stay on the cfb CPU path), which
		 * also keeps fbcon preferring the hardware copy for scrolls.
		 */
		fb_info.flags = FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT;
	} else {
		fb_info.fbops = &macfb_ops;
	}
	fb_info.var		= macfb_defined;
	fb_info.fix		= macfb_fix;
	fb_info.pseudo_palette	= pseudo_palette;

	err = fb_alloc_cmap(&fb_info.cmap, video_cmap_len, 0);
	if (err)
		goto fail_unmap;

	err = register_framebuffer(&fb_info);
	if (err)
		goto fail_dealloc;

	fb_info(&fb_info, "%s frame buffer device\n", fb_info.fix.id);

	return 0;

fail_dealloc:
	fb_dealloc_cmap(&fb_info.cmap);
fail_unmap:
	iounmap(fb_info.screen_base);
	iounmap_macfb();
	return err;
}

module_init(macfb_init);
MODULE_LICENSE("GPL");
