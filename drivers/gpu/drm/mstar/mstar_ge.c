// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_fourcc.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "mstar_ge.h"

static void mstar_get_filltestbuf(void *buf, unsigned int height, unsigned int pitch);

#define DRIVER_NAME "mstar-ge"

#define REG_CTRL		0x0
#define REG_CTRL1		0x4
#define REG_CMQ_STATUS		0x1c
#define REG_IRQ			0x78
#define REG_SRCL		0x80
#define REG_SRCH		0x84
#define REG_DSTL		0x98
#define REG_DSTH		0x9c
#define REG_P256_GB		0xb4
#define REG_P256_AR		0xb8
#define REG_P256_INDEX		0xbc
#define REG_SRCPITCH		0xc0
#define REG_TAGL		0xc4
#define REG_TAGH		0xc8
#define REG_DSTPITCH		0xcc
#define REG_COLORFMT		0xd0
#define REG_CLIP_LEFT		0x154
#define REG_CLIP_RIGHT		0x158
#define REG_CLIP_TOP		0x15c
#define REG_CLIP_BOTTOM		0x160
#define REG_ROT			0x164
#define REG_CMD			0x180
#define REG_X0			0x1a0
#define REG_Y0			0x1a4
#define REG_X1			0x1a8
#define REG_Y1			0x1ac
#define REG_X2			0x1b0
#define REG_Y2			0x1b4
#define REG_BITBLT_SRCWIDTH	0x1b8
#define REG_BITBLT_SRCHEIGHT	0x1bc

static const struct reg_field en_field = REG_FIELD(REG_CTRL, 0, 0);
static const struct reg_field clk_en_field = REG_FIELD(REG_CTRL1, 15, 15);

static const struct reg_field gebusy_field = REG_FIELD(REG_CMQ_STATUS, 0, 0);
static const struct reg_field cmq_free_field = REG_FIELD(REG_CMQ_STATUS, 3, 7);
static const struct reg_field cmq2_free_field = REG_FIELD(REG_CMQ_STATUS, 11, 15);

static const struct reg_field irq_mask_field = REG_FIELD(REG_IRQ, 6, 7);
static const struct reg_field irq_force_field = REG_FIELD(REG_IRQ, 8, 9);
static const struct reg_field irq_clr_field = REG_FIELD(REG_IRQ, 10, 11);
static const struct reg_field irq_status_field = REG_FIELD(REG_IRQ, 12, 13);

static const struct reg_field srcl_field = REG_FIELD(REG_SRCL, 0, 15);
static const struct reg_field srch_field = REG_FIELD(REG_SRCH, 0, 12);

static const struct reg_field dstl_field = REG_FIELD(REG_DSTL, 0, 15);
static const struct reg_field dsth_field = REG_FIELD(REG_DSTH, 0, 12);

static const struct reg_field srcpitch_field = REG_FIELD(REG_SRCPITCH, 0, 13);
static const struct reg_field dstpitch_field = REG_FIELD(REG_DSTPITCH, 0, 13);

static const struct reg_field src_colorfmt_field = REG_FIELD(REG_COLORFMT, 0, 12);
static const struct reg_field dst_colorfmt_field = REG_FIELD(REG_COLORFMT, 8, 12);
#define COLOR_FORMAT_RGB565	0x8
#define COLOR_FORMAT_ARGB8888	0xf

static const struct reg_field clip_left_field = REG_FIELD(REG_CLIP_LEFT, 0, 11);
static const struct reg_field clip_right_field = REG_FIELD(REG_CLIP_RIGHT, 0, 11);
static const struct reg_field clip_top_field = REG_FIELD(REG_CLIP_TOP, 0, 11);
static const struct reg_field clip_bottom_field = REG_FIELD(REG_CLIP_BOTTOM, 0, 11);

static const struct reg_field rot_field = REG_FIELD(REG_ROT, 0, 1);

static const struct reg_field prim_type_field = REG_FIELD(REG_CMD, 4, 6);
#define PRIM_TYPE_LINE		1
#define PRIM_TYPE_RECTFILL	3
#define PRIM_TYPE_BITBLT	4
#define PRIM_TYPE_FREEZE	7

static const struct reg_field x0_field = REG_FIELD(REG_X0, 0, 11);
static const struct reg_field y0_field = REG_FIELD(REG_Y0, 0, 11);
static const struct reg_field x1_field = REG_FIELD(REG_X1, 0, 11);
static const struct reg_field y1_field = REG_FIELD(REG_Y1, 0, 11);
static const struct reg_field x2_field = REG_FIELD(REG_X2, 0, 11);
static const struct reg_field y2_field = REG_FIELD(REG_Y2, 0, 11);

static const struct reg_field bitblt_srcwidth_field = REG_FIELD(REG_BITBLT_SRCWIDTH, 0, 11);
static const struct reg_field bitblt_srcheight_field = REG_FIELD(REG_BITBLT_SRCHEIGHT, 0, 11);

static const struct reg_field tagh_field = REG_FIELD(REG_TAGL, 0, 15);
static const struct reg_field tagl_field = REG_FIELD(REG_TAGH, 0, 15);

static const struct reg_field p256_b_field = REG_FIELD(REG_P256_GB, 0, 7);
static const struct reg_field p256_g_field = REG_FIELD(REG_P256_GB, 8, 15);
static const struct reg_field p256_r_field = REG_FIELD(REG_P256_AR, 0, 7);
static const struct reg_field p256_a_field = REG_FIELD(REG_P256_AR, 8, 15);
static const struct reg_field p256_index_field = REG_FIELD(REG_P256_INDEX, 0, 7);
static const struct reg_field p256_rw_field = REG_FIELD(REG_P256_INDEX, 8, 8);

struct mstar_ge {
	struct device *dev;
	struct drm_device *drm_device;
	struct clk *clk;
	u32 tag;
	struct regmap_field *en, *clk_en, *busy;
	struct regmap_field *cmq_free, *cmq2_free;
	struct regmap_field *irq_mask, *irq_force, *irq_clr, *irq_status;
	struct regmap_field *srcl, *srch;
	struct regmap_field *dstl, *dsth;
	struct regmap_field *srcpitch, *dstpitch;
	struct regmap_field *srcclrfmt, *dstclrfmt;
	struct regmap_field *clip_left, *clip_right, *clip_top, *clip_bottom;
	struct regmap_field *rot;
	struct regmap_field *prim_type;
	struct regmap_field *x0, *y0, *x1, *y1, *x2, *y2;
	struct regmap_field *bitblt_src_width, *bitblt_src_height;
	struct regmap_field *tagl, *tagh;

	/* p256 */
	struct regmap_field *p256_b, *p256_g, *p256_r, *p256_a, *p256_index, *p256_rw;

	struct list_head queue;

	struct miscdevice ge_dev;
};

static const struct regmap_config mstar_ge_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_ge_cmq_free(struct mstar_ge *ge)
{
	unsigned int tmp, cmqfree;

	regmap_field_read(ge->cmq_free, &cmqfree);
	regmap_field_read(ge->cmq2_free, &tmp);
	cmqfree += tmp;

	return cmqfree;
}

static void asciiart(void *bm, int width, int height)
{
	int y;

	for(y = 0; y < height; y++)
		printk("%16ph\n", &((u16*)bm)[width * y]);
}

static void mstar_ge_tag(struct mstar_ge *ge)
{
	ge->tag++;
	regmap_field_force_write(ge->tagl, ge->tag);
	regmap_field_force_write(ge->tagh, ge->tag >> 16);
	dev_info(ge->dev, "tag is: %d\n", (unsigned) ge->tag);
}

static void mstar_ge_set_src(struct mstar_ge *ge, dma_addr_t dmaaddr, unsigned int pitch)
{
	regmap_field_write(ge->srcl, dmaaddr);
	regmap_field_write(ge->srch, dmaaddr >> 16);
	regmap_field_write(ge->srcpitch, pitch);
}

static void mstar_ge_set_dst(struct mstar_ge *ge, dma_addr_t dmaaddr, unsigned int pitch)
{
	regmap_field_write(ge->dstl, dmaaddr);
	regmap_field_write(ge->dsth, dmaaddr >> 16);
	regmap_field_write(ge->dstpitch, pitch);
}

static void mstar_ge_set_clip(struct mstar_ge *ge, unsigned int left, unsigned int top,
		unsigned int right, unsigned int bottom)
{
	regmap_field_write(ge->clip_left, left);
	regmap_field_write(ge->clip_top, top);
	regmap_field_write(ge->clip_right, right);
	regmap_field_write(ge->clip_bottom, bottom);
}

static void mstar_ge_set_priv0(struct mstar_ge *ge, unsigned int x, unsigned int y)
{
	regmap_field_write(ge->x0, x);
	regmap_field_write(ge->y0, y);
}

static void mstar_ge_set_priv1(struct mstar_ge *ge, unsigned int x, unsigned int y)
{
	regmap_field_write(ge->x1, x);
	regmap_field_write(ge->y1, y);
}

static void mstar_ge_set_priv2(struct mstar_ge *ge, unsigned int x, unsigned int y)
{
	regmap_field_write(ge->x2, x);
	regmap_field_write(ge->y2, y);
}

static int mstar_ge_do_line(struct mstar_ge *ge)
{
	void *dst;
	int width = 8, height = 8;
	int psz = 4;
	int pitch = psz * width;
	size_t bufsz = (width * height) * psz;
	dma_addr_t dmadst;
	int ret;

	dev_info(ge->dev, "doing line draw, free %d\n", mstar_ge_cmq_free(ge));

	mstar_ge_tag(ge);

	dst = kzalloc(bufsz, GFP_KERNEL);
	if(!dst)
		return -ENOMEM;

	mstar_get_filltestbuf(dst, height, pitch);

	printk("dst before\n");
	asciiart(dst, width, height);

	dmadst = dma_map_single(ge->dev, dst, bufsz, DMA_FROM_DEVICE);
	ret = dma_mapping_error(ge->dev, dmadst);
	if(ret)
		goto err_free_dst;

	regmap_field_write(ge->en, 1);

	mstar_ge_set_dst(ge, dmadst, pitch);

	mstar_ge_set_clip(ge, 0, 0, width - 1, height - 1);

	mstar_ge_set_priv0(ge, 0, 0);
	mstar_ge_set_priv1(ge, width - 1, height - 1);

	regmap_field_write(ge->bitblt_src_width, width);
	regmap_field_write(ge->bitblt_src_height, height);

	regmap_field_force_write(ge->prim_type, PRIM_TYPE_LINE);
	//regmap_field_write(ge->en, 0);
	mdelay(100);

	dev_info(ge->dev, "line done: %px, %d\n",(void*)dmadst, mstar_ge_cmq_free(ge));

err_unmap_dst:
	dma_unmap_single(ge->dev, dmadst, bufsz, DMA_FROM_DEVICE);

	printk("dest\n");
	asciiart(dst, width, height);

err_free_dst:
	kfree(dst);

	return ret;
}

static int mstar_ge_do_rectfill(struct mstar_ge *ge)
{
	void *dst;
	int width = 8, height = 8;
	int psz = 4;
	int pitch = psz * width;
	size_t bufsz = (width * height) * psz;
	dma_addr_t dmadst;
	int ret;

	dev_info(ge->dev, "doing rect fill, free %d\n", mstar_ge_cmq_free(ge));

	mstar_ge_tag(ge);

	dst = kzalloc(bufsz, GFP_KERNEL);
	if(!dst)
		return -ENOMEM;

	mstar_get_filltestbuf(dst, height, pitch);
	printk("dst before\n");
	asciiart(dst, width, height);

	dmadst = dma_map_single(ge->dev, dst, bufsz, DMA_FROM_DEVICE);
	ret = dma_mapping_error(ge->dev, dmadst);
	if(ret)
		goto err_free_dst;

	regmap_field_write(ge->en, 1);

	mstar_ge_set_dst(ge, dmadst, pitch);
	mstar_ge_set_clip(ge, 0, 0, width - 1, height - 1);
	mstar_ge_set_priv0(ge, 0, 0);
	mstar_ge_set_priv1(ge, (width/2) - 1, (height/2) - 1);

	regmap_field_force_write(ge->prim_type, PRIM_TYPE_RECTFILL);
	//regmap_field_write(ge->en, 0);
	mdelay(100);

	dev_info(ge->dev, "rect fill done: %px, %d\n",(void*)dmadst, mstar_ge_cmq_free(ge));

err_unmap_dst:
	dma_unmap_single(ge->dev, dmadst, bufsz, DMA_FROM_DEVICE);

	printk("dest\n");
	asciiart(dst, width, height);

err_free_dst:
	kfree(dst);

	return ret;
}

static int mstar_ge_drm_color_to_gop(u32 fourcc)
{
	switch(fourcc){
	case DRM_FORMAT_ARGB8888:
		return COLOR_FORMAT_ARGB8888;
	case DRM_FORMAT_RGB565:
		return COLOR_FORMAT_RGB565;
	};

	return -ENOTSUPP;
}

static int mstar_ge_do_bitblt(struct mstar_ge *ge, unsigned int width,
		unsigned int height, struct mstar_ge_bitblt *bitblt)
{
	dev_info(ge->dev, "doing bitblt, free %d\n", mstar_ge_cmq_free(ge));
	mstar_ge_tag(ge);
	regmap_field_write(ge->en, 1);

	regmap_field_write(ge->bitblt_src_width, width);
	regmap_field_write(ge->bitblt_src_height, height);

	regmap_field_write(ge->rot, bitblt->rotation);

	/* set the clip */
	mstar_ge_set_clip(ge, 0, 0, width - 1, height - 1);
	/* set the region to copy from ?*/
	switch(bitblt->rotation){
		case ROTATION_0:
			mstar_ge_set_priv0(ge, 0, 0);
			mstar_ge_set_priv1(ge, width - 1, height - 1);
			break;
		case ROTATION_180:
			mstar_ge_set_priv0(ge, width - 1, height - 1);
			mstar_ge_set_priv1(ge, (width * 2) - 2, (height * 2) - 2);
			break;
	}
	mstar_ge_set_priv2(ge, 0, 0);

	regmap_field_force_write(ge->prim_type, PRIM_TYPE_BITBLT);

	mdelay(100);

	dev_info(ge->dev, "bitblt done\n");
	return 0;
}

#define P256_ENTRIES 0xff

static void mstar_ge_write_p256(struct mstar_ge *ge)
{
	int i;

	for(i = 0; i < P256_ENTRIES; i++){
		regmap_field_write(ge->p256_index, i);
		regmap_field_write(ge->p256_r, 0xff);
		regmap_field_write(ge->p256_g, 0xaa);
		regmap_field_write(ge->p256_b, 0x55);
		regmap_field_write(ge->p256_a, 0xff);
		regmap_field_force_write(ge->p256_rw, 1);
	}
}

static void mstar_ge_read_p256(struct mstar_ge *ge)
{
	unsigned r, g, b, a;
	int i;

	regmap_field_write(ge->p256_rw, 0);

	for(i = 0; i < P256_ENTRIES; i++){
		regmap_field_force_write(ge->p256_index, i);
		regmap_field_read(ge->p256_r, &r);
		regmap_field_read(ge->p256_g, &g);
		regmap_field_read(ge->p256_b, &b);
		regmap_field_read(ge->p256_a, &a);
		dev_warn(ge->dev, "p256 read %x, r: %x, g: %x, b: %x, a: %x\n",
				i, r, g, b, a);
	}
}

int mstar_ge_queue_job(struct mstar_ge *ge, struct mstar_ge_job *job)
{
	/* dst is mandatory */
	if (!job->dst_addr)
		return -EINVAL;

	list_add_tail(&job->queue, &ge->queue);

	/* src is optional */
	if(job->src_addr)
		mstar_ge_set_src(ge, job->src_addr, job->opdata.src_pitch);

	mstar_ge_set_dst(ge, job->dst_addr, job->opdata.dst_pitch);

	regmap_field_write(ge->srcclrfmt, mstar_ge_drm_color_to_gop(job->opdata.src_fourcc) |
			(mstar_ge_drm_color_to_gop(job->opdata.dst_fourcc) << 8));

	switch(job->opdata.op){
	case MSTAR_GE_OP_BITBLT:
		mstar_ge_do_bitblt(ge, job->opdata.src_width, job->opdata.src_height, &job->opdata.bitblt);
		break;
	}

	return 0;
}

static irqreturn_t mstar_ge_irq(int irq, void *data)
{
	struct mstar_ge *ge = data;
	struct mstar_ge_job *job;
	unsigned int status;

	regmap_field_read(ge->irq_status, &status);
	regmap_field_write(ge->irq_force, 0);


	/*
	 * To clear the irq the clear bits need to be
	 * set, but apparently the hardware doesn't clear
	 * them so if we don't clear them no more interrupts
	 * happen..
	 */
	regmap_field_force_write(ge->irq_clr, ~0);
	regmap_field_force_write(ge->irq_clr, 0);

	printk("ge int, %x\n", status);

	if (list_empty(&ge->queue)) {
		dev_err(ge->dev, "Interrupt when no jobs queued!\n");
		return IRQ_NONE;
	}

	job = list_first_entry(&ge->queue, struct mstar_ge_job, queue);

	list_del(&job->queue);

	return IRQ_HANDLED;
}

static void mstar_get_filltestbuf(void *buf, unsigned int height, unsigned int pitch)
{
	int i;
	for(i = 0; i < height; i++)
		memset(buf + (pitch * i), ~i & 0xff, pitch);
}

static int mstar_ge_test_allocbuffers(struct mstar_ge *ge,
		void **src, void **dst, struct mstar_ge_job *j)
{
	*src = kzalloc(mstar_ge_job_srcsz(j), GFP_KERNEL);
	if(!src)
		return -ENOMEM;

	*dst = kzalloc(mstar_ge_job_dstsz(j), GFP_KERNEL);
	if (!*dst) {
		kfree(*src);
		return -ENOMEM;
	}

	return 0;
}

static int mstar_ge_test_mapbuffers(struct mstar_ge *ge, struct mstar_ge_job *j)
{
	int ret;

	j->src_addr = dma_map_single(ge->dev, j->opdata.src, mstar_ge_job_srcsz(j), DMA_TO_DEVICE);
	ret = dma_mapping_error(ge->dev, j->src_addr);
	if(ret)
		return ret;

	j->dst_addr = dma_map_single(ge->dev, j->opdata.dst, mstar_ge_job_dstsz(j), DMA_FROM_DEVICE);
	ret = dma_mapping_error(ge->dev, j->dst_addr);
	if(ret)
		dma_unmap_single(ge->dev, j->dst_addr, mstar_ge_job_srcsz(j), DMA_TO_DEVICE);

	return ret;
}

static void mstar_ge_test_unmapbuffers(struct mstar_ge *ge, struct mstar_ge_job *j)
{
	dma_unmap_single(ge->dev, j->src_addr, mstar_ge_job_dstsz(j), DMA_FROM_DEVICE);
	dma_unmap_single(ge->dev, j->dst_addr, mstar_ge_job_srcsz(j), DMA_TO_DEVICE);
}

static int mstar_ge_test(struct mstar_ge *ge)
{
	struct mstar_ge_job j;
	void *src, *dst;
	int ret;

	mstar_ge_job_init(&j);
	j.opdata.op = MSTAR_GE_OP_BITBLT;
	j.opdata.src_width = 8;
	j.opdata.src_height = 8;
	j.opdata.src_pitch = j.opdata.src_width * 2;
	j.opdata.dst_width = 8;
	j.opdata.dst_height = 8;
	j.opdata.dst_pitch = j.opdata.dst_width * 2;
	j.opdata.src_fourcc = DRM_FORMAT_RGB565;
	j.opdata.dst_fourcc = DRM_FORMAT_RGB565;
	j.opdata.bitblt.rotation = ROTATION_180;

	ret = mstar_ge_test_allocbuffers(ge, &src, &dst, &j);
	if (ret)
		return ret;

	mstar_get_filltestbuf(src, j.opdata.src_height, j.opdata.src_pitch);

	dev_info(ge->dev, "src before\n");
	asciiart(src, j.opdata.src_width, j.opdata.src_height);
	dev_info(ge->dev, "dst before\n");
	asciiart(dst, j.opdata.dst_width, j.opdata.dst_height);

	j.opdata.src = src;
	j.opdata.dst = dst;
	ret = mstar_ge_test_mapbuffers(ge, &j);
	if (ret)
		goto free_src;

	mstar_ge_queue_job(ge, &j);

	mstar_ge_test_unmapbuffers(ge, &j);

	dev_info(ge->dev, "src after job\n");
	asciiart(src, j.opdata.src_width, j.opdata.src_height);
	dev_info(ge->dev, "dst after job\n");
	asciiart(dst, j.opdata.dst_width, j.opdata.dst_height);

free_src:
	kfree(src);
	kfree(dst);

	return ret;
}

static long mstar_ge_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct mstar_ge *ge = container_of(f->private_data, struct mstar_ge, ge_dev);
	int ret = 0;

	switch(cmd){
	case MSTAR_GE_IOCTL_INFO: {
		struct mstar_ge_info info;
		info.caps = 1;
		copy_to_user((void *) arg, &info, sizeof(info));
	}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations ge_fops = {
	.unlocked_ioctl = mstar_ge_ioctl,
};

static int mstar_ge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct mstar_ge *ge;
	void __iomem *base;
	int irq, ret, i;

	ge = devm_kzalloc(dev, sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;

	INIT_LIST_HEAD(&ge->queue);

	ge->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_ge_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ge->en = devm_regmap_field_alloc(dev, regmap, en_field);
	ge->clk_en = devm_regmap_field_alloc(dev, regmap, clk_en_field);
	ge->busy = devm_regmap_field_alloc(dev, regmap, gebusy_field);
	ge->cmq_free = devm_regmap_field_alloc(dev, regmap, cmq_free_field);
	ge->cmq2_free = devm_regmap_field_alloc(dev, regmap, cmq2_free_field);
	ge->irq_mask = devm_regmap_field_alloc(dev, regmap, irq_mask_field);
	ge->irq_force = devm_regmap_field_alloc(dev, regmap, irq_force_field);
	ge->irq_clr = devm_regmap_field_alloc(dev, regmap, irq_clr_field);
	ge->irq_status = devm_regmap_field_alloc(dev, regmap, irq_status_field);

	/* src buffer */
	ge->srcl = devm_regmap_field_alloc(dev, regmap, srcl_field);
	ge->srch = devm_regmap_field_alloc(dev, regmap, srch_field);
	ge->srcpitch = devm_regmap_field_alloc(dev, regmap, srcpitch_field);
	ge->srcclrfmt = devm_regmap_field_alloc(dev, regmap, src_colorfmt_field);

	/* dst buffer */
	ge->dstl = devm_regmap_field_alloc(dev, regmap, dstl_field);
	ge->dsth = devm_regmap_field_alloc(dev, regmap, dsth_field);
	ge->dstpitch = devm_regmap_field_alloc(dev, regmap, dstpitch_field);
	ge->dstclrfmt = devm_regmap_field_alloc(dev, regmap, dst_colorfmt_field);

	/* clipping window */
	ge->clip_left = devm_regmap_field_alloc(dev, regmap, clip_left_field);
	ge->clip_right = devm_regmap_field_alloc(dev, regmap, clip_right_field);
	ge->clip_top = devm_regmap_field_alloc(dev, regmap, clip_top_field);
	ge->clip_bottom = devm_regmap_field_alloc(dev, regmap, clip_bottom_field);

	ge->rot = devm_regmap_field_alloc(dev, regmap, rot_field);
	ge->prim_type = devm_regmap_field_alloc(dev, regmap, prim_type_field);

	/* vertex */
	ge->x0 = devm_regmap_field_alloc(dev, regmap, x0_field);
	ge->y0 = devm_regmap_field_alloc(dev, regmap, y0_field);
	ge->x1 = devm_regmap_field_alloc(dev, regmap, x1_field);
	ge->y1 = devm_regmap_field_alloc(dev, regmap, y1_field);
	ge->x2 = devm_regmap_field_alloc(dev, regmap, x2_field);
	ge->y2 = devm_regmap_field_alloc(dev, regmap, y2_field);

	ge->bitblt_src_width = devm_regmap_field_alloc(dev, regmap, bitblt_srcwidth_field);
	ge->bitblt_src_height = devm_regmap_field_alloc(dev, regmap, bitblt_srcheight_field);

	ge->tagl = devm_regmap_field_alloc(dev, regmap, tagl_field);
	ge->tagh = devm_regmap_field_alloc(dev, regmap, tagh_field);

	/* p256 */
	ge->p256_b = devm_regmap_field_alloc(dev, regmap, p256_b_field);
	ge->p256_g = devm_regmap_field_alloc(dev, regmap, p256_g_field);
	ge->p256_r = devm_regmap_field_alloc(dev, regmap, p256_r_field);
	ge->p256_a = devm_regmap_field_alloc(dev, regmap, p256_a_field);
	ge->p256_index = devm_regmap_field_alloc(dev, regmap, p256_index_field);
	ge->p256_rw = devm_regmap_field_alloc(dev, regmap, p256_rw_field);

	ge->clk = devm_clk_get(dev, "ge");
	if (IS_ERR(ge->clk))
		return PTR_ERR(ge->clk);

	clk_prepare_enable(ge->clk);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -ENODEV;

	ret = devm_request_irq(dev, irq, mstar_ge_irq, IRQF_SHARED, dev_name(dev), ge);
	if (ret)
		return ret;

	regmap_field_write(ge->irq_mask, 0);
	regmap_field_write(ge->clk_en, 1);

	dev_set_drvdata(dev, ge);

	mstar_ge_test(ge);

	ge->ge_dev.minor = MISC_DYNAMIC_MINOR;
	ge->ge_dev.name	= DRIVER_NAME;
	ge->ge_dev.fops	= &ge_fops;

	return misc_register(&ge->ge_dev);
}

static int mstar_ge_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_ge *ge = dev_get_drvdata(dev);

	misc_deregister(&ge->ge_dev);

	return 0;
}

static const struct of_device_id mstar_ge_ids[] = {
	{
		.compatible = "sstar,ssd20xd-ge",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_ge_ids);

static struct platform_driver mstar_ge_driver = {
	.probe = mstar_ge_probe,
	.remove = mstar_ge_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_ge_ids,
	},
};
module_platform_driver(mstar_ge_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
