#include <asm/io.h>
#include <linux/console.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

#include <asm/vdp.h>
#include <asm/tuxhead.h>

static struct tty_driver *vdp_tty_driver = NULL;
static struct tty_port vdp_port;
static int vdp_irq = -1;

#define VDP_START_Y 4

/* Nametable height in tiles (a 256px plane); the visible area is
 * VDP_PLANE_AB_HEIGHT rows.  The plane is used as a circular buffer and the
 * VDP's vertical scroll (VSRAM) slides the visible window over it, so a
 * scroll is just a scroll-register update plus clearing one new line. */
#define VDP_PLANE_ROWS 32

static unsigned int vdp_cur_x = 0;
static unsigned int vdp_cur_y = VDP_START_Y;
static unsigned int vdp_top   = 0;	/* nametable row shown at screen top */

/* Serialises the shared cursor/scroll state (device_lock for the nbcon
 * console, and the tty write path). */
static DEFINE_SPINLOCK(vdp_lock);

/* Top status band rendered on the (non-scrolling) window plane. */
#define VDP_WIN_ROWS       VDP_START_Y     /* height in tiles, top of screen */
#define VDP_WIN_PAL        1               /* CRAM palette line for the band */
#define VDP_WIN_TILE0      0x80            /* first of 16 solid-colour tiles */

/*
 * Window colour palette.  Each colour is an index (0-15) into VDP_WIN_PAL;
 * a solid tile is built per index so a cell can show any of these as a flat
 * fill.  Index 0 maps to pixel 0 and is therefore transparent (lets the text
 * plane show through); 1-15 are opaque.
 */
#define VDP_COL_TRANSPARENT 0
#define VDP_COL_BLACK       1
#define VDP_COL_WHITE       2
#define VDP_COL_RED         3
#define VDP_COL_GREEN       4
#define VDP_COL_BLUE        5
#define VDP_COL_YELLOW      6
#define VDP_COL_CYAN        7
#define VDP_COL_MAGENTA     8
#define VDP_COL_GRAY        9
#define VDP_COL_DARKGRAY    10
#define VDP_COL_ORANGE      11
#define VDP_COL_PURPLE      12
#define VDP_COL_BROWN       13
#define VDP_COL_PINK        14
#define VDP_COL_LIME        15

static const u16 vdp_default_palette[16] = {
	[VDP_COL_TRANSPARENT] = VDP_RGB(0, 0, 0),
	[VDP_COL_BLACK]       = VDP_RGB(0, 0, 0),
	[VDP_COL_WHITE]       = VDP_RGB(7, 7, 7),
	[VDP_COL_RED]         = VDP_RGB(7, 0, 0),
	[VDP_COL_GREEN]       = VDP_RGB(0, 7, 0),
	[VDP_COL_BLUE]        = VDP_RGB(0, 0, 7),
	[VDP_COL_YELLOW]      = VDP_RGB(7, 7, 0),
	[VDP_COL_CYAN]        = VDP_RGB(0, 7, 7),
	[VDP_COL_MAGENTA]     = VDP_RGB(7, 0, 7),
	[VDP_COL_GRAY]        = VDP_RGB(3, 3, 3),
	[VDP_COL_DARKGRAY]    = VDP_RGB(1, 1, 1),
	[VDP_COL_ORANGE]      = VDP_RGB(7, 3, 0),
	[VDP_COL_PURPLE]      = VDP_RGB(4, 0, 5),
	[VDP_COL_BROWN]       = VDP_RGB(3, 1, 0),
	[VDP_COL_PINK]        = VDP_RGB(7, 3, 5),
	[VDP_COL_LIME]        = VDP_RGB(5, 7, 0),
};

/* Set window cell (col, row) to a palette colour.  Caller holds vdp_lock. */
static inline void __vdp_window_set_color(unsigned int col, unsigned int row,
					  u8 colour)
{
	u16 entry = (VDP_WIN_PAL << 13) | (VDP_WIN_TILE0 + (colour & 0x0F));

	vdp_vram_set_addr(VRAM_WINDOW +
			  (row * VDP_PLANE_AB_WIDTH + col) * 2);
	vdp_data_write(entry);
}

/* Set the colour of the window cell at (col, row); see VDP_COL_* above. */
static void vdp_window_set_color(unsigned int col, unsigned int row, u8 colour)
{
	unsigned long flags;

	spin_lock_irqsave(&vdp_lock, flags);
	__vdp_window_set_color(col, row, colour);
	spin_unlock_irqrestore(&vdp_lock, flags);
}

/*
 * Configure the window plane as a fixed, full-width band across the top
 * VDP_WIN_ROWS rows.  The window replaces plane A there and does not scroll,
 * so it stays put while the text plane scrolls underneath.  Builds the solid
 * colour tiles and palette, then fills the band black.
 */
static void vdp_window_setup(void)
{
	unsigned int i, w, row, col;
	unsigned long flags;

	spin_lock_irqsave(&vdp_lock, flags);

	/* 16 solid tiles: tile VDP_WIN_TILE0+k has every pixel = index k. */
	vdp_register_set(15, 2);                /* auto-increment by 2 */
	vdp_vram_set_addr(VDP_WIN_TILE0 * 32);
	for (i = 0; i < 16; i++)
		for (w = 0; w < 16; w++)        /* 32 bytes = 16 words/tile */
			vdp_data_write(i * 0x1111);

	/* Load the window palette into CRAM line VDP_WIN_PAL. */
	vdp_cram_set_addr(VDP_WIN_PAL * 16 * 2);
	for (i = 0; i < 16; i++)
		vdp_data_write(vdp_default_palette[i]);

	/* Fill the band black. */
	for (row = 0; row < VDP_WIN_ROWS; row++)
		for (col = 0; col < VDP_PLANE_AB_WIDTH; col++)
			__vdp_window_set_color(col, row, VDP_COL_BLACK);

	/* reg3:  window nametable base = VRAM_WINDOW.
	 * reg17: 0 -> no horizontal split (full width).
	 * reg18: VDP_WIN_ROWS, DOWN=0 -> window covers rows 0..VDP_WIN_ROWS-1. */
	vdp_register_set(3, VRAM_WINDOW >> 10);
	vdp_register_set(17, 0x00);
	vdp_register_set(18, VDP_WIN_ROWS);

	spin_unlock_irqrestore(&vdp_lock, flags);
}

/*
 * Liveness heartbeat: blink the top-right window cell.  Driven by a kernel
 * timer, so it keeps beating only while the timer/softirq machinery is making
 * progress and freezes if the system wedges.
 */
#define VDP_HB_COL     (VDP_PLANE_AB_WIDTH - 1)
#define VDP_HB_ROW     0
#define VDP_HB_PERIOD  msecs_to_jiffies(500)   /* toggle twice a second */

static struct timer_list vdp_heartbeat_timer;
static bool vdp_heartbeat_on;

static void vdp_heartbeat(struct timer_list *t)
{
	vdp_heartbeat_on = !vdp_heartbeat_on;
	vdp_window_set_color(VDP_HB_COL, VDP_HB_ROW,
			     vdp_heartbeat_on ? VDP_COL_GREEN : VDP_COL_BLACK);

	mod_timer(&vdp_heartbeat_timer, jiffies + VDP_HB_PERIOD);
}

/*
 * Disk-activity indicator: the cell directly below the heartbeat.  Each
 * Everdrive SD/file access lights it; a one-shot timer, re-armed on every
 * access, clears it once I/O has been quiet for VDP_DISK_HOLD.  So brief
 * accesses flash and sustained I/O stays lit, like a drive-activity LED.
 * The everdrive driver calls vdp_disk_activity() once per access.
 */
#define VDP_DISK_COL   VDP_HB_COL             /* same column as the heartbeat */
#define VDP_DISK_ROW   (VDP_HB_ROW + 1)       /* the tile directly beneath it */
#define VDP_DISK_HOLD  msecs_to_jiffies(120)  /* linger after the last access */

static void vdp_disk_idle(struct timer_list *t);
static DEFINE_TIMER(vdp_disk_timer, vdp_disk_idle);
static bool vdp_disk_lit;

static void vdp_disk_idle(struct timer_list *t)
{
	WRITE_ONCE(vdp_disk_lit, false);
	vdp_window_set_color(VDP_DISK_COL, VDP_DISK_ROW, VDP_COL_BLACK);
}

void vdp_disk_activity(void)
{
	/* Only paint on the leading edge; just re-arm while I/O continues. */
	if (!READ_ONCE(vdp_disk_lit)) {
		WRITE_ONCE(vdp_disk_lit, true);
		vdp_window_set_color(VDP_DISK_COL, VDP_DISK_ROW, VDP_COL_RED);
	}

	mod_timer(&vdp_disk_timer, jiffies + VDP_DISK_HOLD);
}
EXPORT_SYMBOL_GPL(vdp_disk_activity);

/* Logo: 4x4 tiles at the top-left of the window. */
#define VDP_LOGO_W      4
#define VDP_LOGO_H      4
#define VDP_LOGO_TILES  (VDP_LOGO_W * VDP_LOGO_H)
#define VDP_LOGO_TILE0  0x90   /* VRAM tile slot (after the 0x80-0x8F band tiles) */
#define VDP_LOGO_PAL    2      /* CRAM palette line for the logo (font=0, band=1) */

/*
 * Upload a 32x32 (4x4 tile) logo to the top-left of the window plane.
 * @tiledata: 4bpp pixel data, 32 bytes per tile, VDP_LOGO_TILES tiles.
 * @tilemap:  VDP_LOGO_TILES nametable entries (row-major 4x4).  Each entry's
 *            tile-index field (bits 0-10) is relative to @tiledata and is
 *            rebased onto VDP_LOGO_TILE0; flip/priority bits are kept but the
 *            palette is forced to VDP_LOGO_PAL.
 * @palette:  16 colours loaded into CRAM line VDP_LOGO_PAL.
 * Call after the window is set up.
 */
static void vdp_load_logo(const u8 *tiledata, const u16 *tilemap,
			  const u16 *palette)
{
	const u8 *p = tiledata;
	unsigned int i, row, col;
	unsigned long flags;

	spin_lock_irqsave(&vdp_lock, flags);

	/* Tile pixel data (big-endian word stream into VRAM). */
	vdp_register_set(15, 2);                /* auto-increment by 2 */
	vdp_vram_set_addr(VDP_LOGO_TILE0 * 32);
	for (i = 0; i < VDP_LOGO_TILES * 32; i += 2)
		vdp_data_write((p[i] << 8) | p[i + 1]);

	/* Logo palette -> CRAM line VDP_LOGO_PAL. */
	vdp_cram_set_addr(VDP_LOGO_PAL * 16 * 2);
	for (i = 0; i < 16; i++)
		vdp_data_write(palette[i]);

	/* Tile map -> top-left VDP_LOGO_H x VDP_LOGO_W cells of the window. */
	for (row = 0; row < VDP_LOGO_H; row++) {
		for (col = 0; col < VDP_LOGO_W; col++) {
			u16 e = tilemap[row * VDP_LOGO_W + col];
			u16 tile = ((e & 0x07FF) + VDP_LOGO_TILE0) & 0x07FF;
			/* keep priority(15)/flip(11,12), force palette(13,14) */
			u16 out = (e & 0x9800) | (VDP_LOGO_PAL << 13) | tile;

			vdp_vram_set_addr(VRAM_WINDOW +
					  (row * VDP_PLANE_AB_WIDTH + col) * 2);
			vdp_data_write(out);
		}
	}

	spin_unlock_irqrestore(&vdp_lock, flags);
}

/* Write a character at screen position (x, screen_y) via the circular plane. */
static inline void vdp_putc_at(char ch, unsigned int x, unsigned int screen_y)
{
	unsigned int row = (vdp_top + screen_y) % VDP_PLANE_ROWS;

	vdp_setc(ch, row * VDP_PLANE_AB_WIDTH + x);
}

static inline void vdp_next_line(void)
{
	unsigned int x, row;

	vdp_cur_x = 0;

	if (vdp_cur_y < VDP_PLANE_AB_HEIGHT - 1) {
		vdp_cur_y++;
		return;
	}

	/* At the bottom: scroll up one row in hardware, then clear the line
	 * that has just wrapped in at the bottom of the visible window. */
	vdp_top = (vdp_top + 1) % VDP_PLANE_ROWS;
	vdp_set_vscroll(vdp_top * 8);

	row = (vdp_top + VDP_PLANE_AB_HEIGHT - 1) % VDP_PLANE_ROWS;
	for (x = 0; x < VDP_PLANE_AB_WIDTH; x++)
		vdp_setc(' ', row * VDP_PLANE_AB_WIDTH + x);

	/* Keep the top VDP_START_Y rows (logo area) clear: the scroll just slid
	 * the old text top into the bottom of the masked band, so erase it. */
	row = (vdp_top + VDP_START_Y - 1) % VDP_PLANE_ROWS;
	for (x = 0; x < VDP_PLANE_AB_WIDTH; x++)
		vdp_setc(' ', row * VDP_PLANE_AB_WIDTH + x);
}

static void vdp_puts(const char *buf, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		char ch = *buf++;

		if (ch == '\n') {
			vdp_next_line();
			continue;
		}

		vdp_putc_at(ch, vdp_cur_x, vdp_cur_y);

		vdp_cur_x += 1;
		if (vdp_cur_x == VDP_PLANE_AB_WIDTH)
			vdp_next_line();
	}
}

static int vdp_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(&vdp_port, tty, filp);
}

static void vdp_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(&vdp_port, tty, filp);
}

static ssize_t vdp_tty_write(struct tty_struct *tty, const unsigned char *buf, size_t count)
{
	unsigned long flags;

	spin_lock_irqsave(&vdp_lock, flags);
	vdp_puts(buf, count);
	spin_unlock_irqrestore(&vdp_lock, flags);
	return count;
}

static unsigned int vdp_tty_write_room(struct tty_struct *tty)
{
	/* Let a line in at a time */
        return VDP_PLANE_AB_WIDTH;
}

static const struct tty_operations vdp_tty_ops = {
        .open       = vdp_tty_open,
        .close      = vdp_tty_close,
        .write      = vdp_tty_write,
        .write_room = vdp_tty_write_room,
};

static irqreturn_t megadrive_vdp_irq_handler(int irq, void *dev_id)
{
        return IRQ_HANDLED;
}

static int __init megadrive_vdp_init(void)
{
        struct device_node *np;
        int ret;

        np = of_find_compatible_node(NULL, NULL, "sega,megadrive-vdp");
        if (!np)
                return 0;

        vdp_irq = of_irq_get(np, 0);
        of_node_put(np);

        if (vdp_irq < 0) {
                pr_err("megadrive-vdp: failed to get IRQ: %d\n", vdp_irq);
                return vdp_irq;
        }

        ret = request_irq(vdp_irq, megadrive_vdp_irq_handler, 0,
                          "megadrive-vdp", NULL);
        if (ret) {
                pr_err("megadrive-vdp: failed to request IRQ %d: %d\n",
                       vdp_irq, ret);
                return ret;
        }

        tty_port_init(&vdp_port);

        vdp_tty_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW |
                                             TTY_DRIVER_DYNAMIC_DEV);
        if (IS_ERR(vdp_tty_driver)) {
                ret = PTR_ERR(vdp_tty_driver);
                goto err_free_irq;
        }

        vdp_tty_driver->driver_name  = "ttyVDP";
        vdp_tty_driver->name         = "ttyVDP";
        vdp_tty_driver->type         = TTY_DRIVER_TYPE_SERIAL;
        vdp_tty_driver->subtype      = SERIAL_TYPE_NORMAL;
        vdp_tty_driver->init_termios = tty_std_termios;
        tty_set_operations(vdp_tty_driver, &vdp_tty_ops);
        tty_port_link_device(&vdp_port, vdp_tty_driver, 0);
        ret = tty_register_driver(vdp_tty_driver);
        if (ret) {
                tty_driver_kref_put(vdp_tty_driver);
                tty_port_destroy(&vdp_port);
                goto err_free_irq;
        }

        tty_register_device(vdp_tty_driver, 0, NULL);

        timer_setup(&vdp_heartbeat_timer, vdp_heartbeat, 0);
        mod_timer(&vdp_heartbeat_timer, jiffies + VDP_HB_PERIOD);

        return 0;

err_free_irq:
        free_irq(vdp_irq, NULL);
        tty_port_destroy(&vdp_port);
        return ret;
}

static void __exit megadrive_vdp_exit(void)
{
        if (!vdp_tty_driver)
                return;

        timer_delete_sync(&vdp_heartbeat_timer);
        timer_delete_sync(&vdp_disk_timer);
        tty_unregister_device(vdp_tty_driver, 0);
        free_irq(vdp_irq, NULL);
        tty_unregister_driver(vdp_tty_driver);
        tty_driver_kref_put(vdp_tty_driver);
        tty_port_destroy(&vdp_port);
}

module_init(megadrive_vdp_init);
module_exit(megadrive_vdp_exit);

static void vdp_console_emit(struct nbcon_write_context *wctxt)
{
	if (!nbcon_enter_unsafe(wctxt))
		return;

	vdp_puts(wctxt->outbuf, wctxt->len);

	nbcon_exit_unsafe(wctxt);
}

static void vdp_console_write_atomic(struct console *co,
				     struct nbcon_write_context *wctxt)
{
	vdp_console_emit(wctxt);
}

static void vdp_console_write_thread(struct console *co,
				     struct nbcon_write_context *wctxt)
{
	vdp_console_emit(wctxt);
}

static void vdp_console_device_lock(struct console *co, unsigned long *flags)
{
	spin_lock_irqsave(&vdp_lock, *flags);
}

static void vdp_console_device_unlock(struct console *co, unsigned long flags)
{
	spin_unlock_irqrestore(&vdp_lock, flags);
}

static struct tty_driver *vdp_console_device(struct console *co, int *index)
{
	*index = 0;
	return vdp_tty_driver;
}

static int vdp_console_setup(struct console *co, char *options)
{
	return 0;
}

static struct console vdp_console = {
        .name          = "ttyVDP",
        .write_atomic  = vdp_console_write_atomic,
        .write_thread  = vdp_console_write_thread,
        .device_lock   = vdp_console_device_lock,
        .device_unlock = vdp_console_device_unlock,
        .device        = vdp_console_device,
        .setup         = vdp_console_setup,
        .flags         = CON_PRINTBUFFER | CON_NBCON,
        .index         = 0,
};

/*
 * Disable all sprites (e.g. one left enabled by U-Boot, which would otherwise
 * draw on top of the window logo).  Point the sprite attribute table at a
 * known slot, then make sprite 0 sit off the top edge with a terminated link
 * so the VDP's sprite walk stops immediately and nothing is drawn.
 */
static void vdp_disable_sprites(void)
{
	unsigned long flags;

	spin_lock_irqsave(&vdp_lock, flags);

	vdp_register_set(5, VRAM_SPRITE_TABLE >> 9);   /* SAT base            */
	vdp_register_set(15, 2);                       /* auto-increment by 2 */
	vdp_vram_set_addr(VRAM_SPRITE_TABLE);
	vdp_data_write(0x0000);   /* sprite 0: Y = 0 (off-screen)        */
	vdp_data_write(0x0000);   /* size = 1x1, link = 0 (end of chain) */

	spin_unlock_irqrestore(&vdp_lock, flags);
}

static int __init everdrive_console_init(void)
{
        struct device_node *np;

        np = of_find_compatible_node(NULL, NULL, "sega,megadrive-vdp");
        if (!np)
                return 0;

        of_node_put(np);

        vdp_window_setup();

        vdp_disable_sprites();

        vdp_load_logo(tuxhead_tiles, (u16 *) tuxhead_tilemap,
                      (u16 *) tuxhead_pal);

        register_console(&vdp_console);

        return 0;
}

console_initcall(everdrive_console_init);

MODULE_LICENSE("GPL");
