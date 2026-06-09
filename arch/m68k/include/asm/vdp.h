#ifndef _VDP_H
#define _VDP_H

#define VDP_DATA	0xc00000U
#define VDP_CTRL	0xc00004U
#define VDP_CTRL_CD0	BIT(30)
#define VDP_CTRL_CD1	BIT(31)
#define VDP_CTRL_CD2	BIT(4)
#define VDP_CTRL_CD3	BIT(5)
#define VDP_CTRL_CD4	BIT(6)
#define VDP_CTRL_CD5	BIT(7)

#define VRAM_WINDOW		0xB000
#define VRAM_PLANE_A		0xC000
#define VRAM_PLANE_B		0xE000
#define VRAM_SPRITE_TABLE	0xF000
#define VRAM_HSCROLL		0xF800

#define VDP_PLANE_AB_WIDTH  32
#define VDP_PLANE_AB_HEIGHT 28
#define VDP_TILE_WIDTH 8

/* Build a VDP colour from 3-bit (0-7) R, G, B components (MD BGR444). */
#define VDP_RGB(r, g, b)	(((r) << 1) | ((g) << 5) | ((b) << 9))

#ifndef __ASSEMBLY__
static inline void vdp_vram_set_addr(u16 addr)
{
	u32 _addr = addr;
	u32 v;

	v = (_addr & 0x3fff) << 16;
	v |= (_addr & 0xc000) >> 14;

	/* write */
	v |=  VDP_CTRL_CD0;

	iowrite32be(v, (void *) VDP_CTRL);
}

static inline void vdp_data_write(u16 value)
{
	iowrite16be(value, (void *) VDP_DATA);
}

static inline void vdp_vsram_set_addr(u16 addr)
{
	u32 _addr = addr;
	u32 v;

	v = (_addr & 0x3fff) << 16;
	v |= (_addr & 0xc000) >> 14;

	/* VSRAM write */
	v |= VDP_CTRL_CD0 | VDP_CTRL_CD2;

	iowrite32be(v, (void *) VDP_CTRL);
}

static inline void vdp_cram_set_addr(u16 addr)
{
	u32 _addr = addr;
	u32 v;

	v = (_addr & 0x3fff) << 16;
	v |= (_addr & 0xc000) >> 14;

	/* CRAM write */
	v |= VDP_CTRL_CD0 | VDP_CTRL_CD1;

	iowrite32be(v, (void *) VDP_CTRL);
}

static inline void vdp_set_vscroll(u16 px)
{
	/* plane B vertical scroll is VSRAM entry 1 (byte address 2) */
	vdp_vsram_set_addr(2);
	vdp_data_write(px);
}

static inline void vdp_set_tile(u16 plane_addr, u16 plane_index, u8 tile_index)
{
	uint16_t tmp;

	tmp = tile_index;

	vdp_vram_set_addr(plane_addr + (plane_index * 2));
	vdp_data_write(tmp);
}

static void vdp_setc(char ch, u16 idx)
{
	vdp_set_tile(VRAM_PLANE_B, idx, ch);
}


static inline void vdp_register_set(u8 reg, u8 value)
{
	u16 _reg = reg;
	u16 tmp = (0x8000 | (_reg << 8) | value);
	iowrite16be(tmp, (void *) VDP_CTRL);
}

static inline u16 vdp_read_status(void)
{
	return ioread16be((void *) VDP_CTRL);
}

/* Pulse the on-screen disk-activity indicator (defined in the VDP driver).
 * Call once per Everdrive SD/file access. */
void vdp_disk_activity(void);

#endif

#endif /* _VDP_H */
