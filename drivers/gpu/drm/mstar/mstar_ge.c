// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/clk.h>

#define DRIVER_NAME "mstar-ge"

#define REG_CTRL		0x0
#define REG_IRQ			0x78
#define REG_SRCL		0x80
#define REG_SRCH		0x84
#define REG_DSTL		0x98
#define REG_DSTH		0x9c
#define REG_SRCPITCH		0xc0
#define REG_DSTPITCH		0xcc
#define REG_COLORFMT		0xd0
#define REG_ROT			0x164
#define REG_CMD			0x180
#define REG_BITBLT_SRCWIDTH	0x1b8
#define REG_BITBLT_SRCHEIGHT	0x1bc

static const struct reg_field en_field = REG_FIELD(REG_CTRL, 0, 0);

static const struct reg_field irq_mask_field = REG_FIELD(REG_IRQ, 6, 7);
static const struct reg_field irq_force_field = REG_FIELD(REG_IRQ, 8, 9);
static const struct reg_field irq_clr_field = REG_FIELD(REG_IRQ, 10, 11);
static const struct reg_field irq_status_field = REG_FIELD(REG_IRQ, 12, 13);

static const struct reg_field srcl_field = REG_FIELD(REG_SRCL, 0, 15);
static const struct reg_field srch_field = REG_FIELD(REG_SRCH, 0, 7);

static const struct reg_field dstl_field = REG_FIELD(REG_DSTL, 0, 15);
static const struct reg_field dsth_field = REG_FIELD(REG_DSTH, 0, 7);

static const struct reg_field srcpitch_field = REG_FIELD(REG_SRCPITCH, 0, 13);
static const struct reg_field dstpitch_field = REG_FIELD(REG_DSTPITCH, 0, 13);

static const struct reg_field src_colorfmt_field = REG_FIELD(REG_COLORFMT, 0, 4);
static const struct reg_field dst_colorfmt_field = REG_FIELD(REG_COLORFMT, 8, 12);
#define COLOR_FORMAT_ARGB8888	0xf

static const struct reg_field rot_field = REG_FIELD(REG_ROT, 0, 1);
#define ROTATION_0	0
#define ROTATION_90	1
#define ROTATION_180	2
#define ROTATION_270	3

static const struct reg_field prim_type_field = REG_FIELD(REG_CMD, 4, 6);
#define PRIM_TYPE_LINE		1
#define PRIM_TYPE_RECTFILL	3
#define PRIM_TYPE_BITBLT	4
#define PRIM_TYPE_FREEZE	7

static const struct reg_field bitblt_srcwidth_field = REG_FIELD(REG_BITBLT_SRCWIDTH, 0, 11);
static const struct reg_field bitblt_srcheight_field = REG_FIELD(REG_BITBLT_SRCHEIGHT, 0, 11);

struct mstar_ge {
	struct device *dev;
	struct clk *clk;
	struct regmap_field *en;
	struct regmap_field *irq_mask, *irq_force, *irq_clr, *irq_status;
	struct regmap_field *srcl, *srch;
	struct regmap_field *dstl, *dsth;
	struct regmap_field *srcpitch, *dstpitch;
	struct regmap_field *srcclrfmt, *dstclrfmt;
	struct regmap_field *rot;
	struct regmap_field *prim_type;
	struct regmap_field *bitblt_src_width, *bitblt_src_height;
};

static const struct regmap_config mstar_ge_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_ge_do_bitblt(struct mstar_ge *ge)
{
	void *src, *dst;
	int width = 128, height = 128;
	int psz = 4;
	int pitch = psz * width;
	size_t bufsz = (width * height) * psz;
	dma_addr_t dmasrc, dmadst;
	int ret;

	dev_info(ge->dev, "doing bitblt\n");

	src = kzalloc(bufsz, GFP_KERNEL);
	if(!src)
		return -ENOMEM;
	dst = kzalloc(bufsz, GFP_KERNEL);
	if(!dst)
		return -ENOMEM;

	dmasrc = dma_map_single(ge->dev, src, bufsz, DMA_TO_DEVICE);
	ret = dma_mapping_error(ge->dev, dmasrc);
	if(ret)
		return ret;

	dmadst = dma_map_single(ge->dev, dst, bufsz, DMA_TO_DEVICE);
	ret = dma_mapping_error(ge->dev, dmadst);
	if(ret)
		goto err_unmap_src;

	regmap_field_write(ge->en, 1);

	regmap_field_write(ge->srcl, dmasrc);
	regmap_field_write(ge->srch, dmasrc >> 16);
	regmap_field_write(ge->dstl, dmadst);
	regmap_field_write(ge->dsth, dmadst >> 16);
	regmap_field_write(ge->srcpitch, pitch);
	regmap_field_write(ge->dstpitch, pitch);
	regmap_field_write(ge->srcclrfmt, COLOR_FORMAT_ARGB8888);
	regmap_field_write(ge->dstclrfmt, COLOR_FORMAT_ARGB8888);

	regmap_field_write(ge->prim_type, PRIM_TYPE_BITBLT);
	//regmap_field_write(ge->en, 0);

	dev_info(ge->dev, "bitblt done: %px %px\n", dmasrc, dmadst);

	kfree(src);
	kfree(dst);

err_unmap_dst:
	dma_unmap_single(ge->dev, dmadst, bufsz, DMA_TO_DEVICE);
err_unmap_src:
	dma_unmap_single(ge->dev, dmasrc, bufsz, DMA_TO_DEVICE);

	return ret;
}

static int mstar_ge_bind(struct device *dev, struct device *master, void *data)
{
	return 0;
}

static void mstar_ge_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mstar_ge_component_ops = {
	.bind	= mstar_ge_bind,
	.unbind	= mstar_ge_unbind,
};

static irqreturn_t mstar_ge_irq(int irq, void *data)
{
	struct mstar_ge *ge = data;
	unsigned int status;

	regmap_field_read(ge->irq_status, &status);

	printk("ge int, %x\n", status);

	regmap_field_write(ge->irq_force, 0);
	regmap_field_write(ge->irq_clr, ~0);

	return IRQ_HANDLED;
}

static int mstar_ge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct mstar_ge *ge;
	void __iomem *base;
	int irq, ret;

	ge = devm_kzalloc(dev, sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;

	ge->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_ge_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ge->en = devm_regmap_field_alloc(dev, regmap, en_field);
	ge->irq_mask = devm_regmap_field_alloc(dev, regmap, irq_mask_field);
	ge->irq_force = devm_regmap_field_alloc(dev, regmap, irq_force_field);
	ge->irq_clr = devm_regmap_field_alloc(dev, regmap, irq_clr_field);
	ge->irq_status = devm_regmap_field_alloc(dev, regmap, irq_status_field);
	ge->srcl = devm_regmap_field_alloc(dev, regmap, srcl_field);
	ge->srch = devm_regmap_field_alloc(dev, regmap, srch_field);
	ge->dstl = devm_regmap_field_alloc(dev, regmap, dstl_field);
	ge->dsth = devm_regmap_field_alloc(dev, regmap, dsth_field);
	ge->srcpitch = devm_regmap_field_alloc(dev, regmap, srcpitch_field);
	ge->dstpitch = devm_regmap_field_alloc(dev, regmap, dstpitch_field);
	ge->srcclrfmt = devm_regmap_field_alloc(dev, regmap, src_colorfmt_field);
	ge->dstclrfmt = devm_regmap_field_alloc(dev, regmap, dst_colorfmt_field);
	ge->rot = devm_regmap_field_alloc(dev, regmap, rot_field);
	ge->prim_type = devm_regmap_field_alloc(dev, regmap, prim_type_field);
	ge->bitblt_src_width = devm_regmap_field_alloc(dev, regmap, bitblt_srcwidth_field);
	ge->bitblt_src_height = devm_regmap_field_alloc(dev, regmap, bitblt_srcheight_field);

	ge->clk = devm_clk_get(dev, "ge");
	if (IS_ERR(ge->clk))
		return PTR_ERR(ge->clk);

	clk_prepare_enable(ge->clk);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -ENODEV;

	regmap_field_write(ge->irq_mask, 0);
	ret = devm_request_irq(dev, irq, mstar_ge_irq, IRQF_SHARED, dev_name(dev), ge);
	if (ret)
		return ret;

	dev_set_drvdata(dev, ge);

	mstar_ge_do_bitblt(ge);

	return component_add(&pdev->dev, &mstar_ge_component_ops);
}

static int mstar_ge_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_ge_component_ops);
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
