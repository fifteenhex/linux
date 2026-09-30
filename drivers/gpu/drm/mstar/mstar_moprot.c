// SPDX-License-Identifier: GPL-2.0-or-later
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME "mstar-moprot"

struct mstar_moprot {
	struct regmap *regmap;
};

/*
 * The MOP output stage, and the two NV12 rotators that live in the same page.
 *
 * Without this init the MOP's windows can be programmed and enabled and the
 * plane still never reaches the display mixer, so nothing of it appears on the
 * panel - which is why the graphics plane looked dead in Linux while the GOP
 * worked. The same block is in u-boot's mstar_mop.c (mop_output[]).
 *
 * It is HalDispMopRotInit(): four register groups 0x40 apart, each given the
 * same three values at +0x08, +0x0c and +0x18, then HalDispMopRot0DbfEn(1) and
 * HalDispMopRot1DbfEn(1), which are bits 0 and 1 of 0x004. So the four groups
 * are the rotators, and the reason this makes the output path work is that the
 * MOP's output runs through them whether or not anything is being rotated.
 *
 * The rotators, from mhal's HalDispMopRot0 and Rot1 accessors, confirmed on an SSD202D:
 *
 *   0x004  bit 0  rotator 0 double buffering enable
 *          bit 1  rotator 1 double buffering enable
 *          bit 2  rotator 0 latch: set then clear (HalDispMopRotDbBfWr)
 *          bit 3  rotator 1 latch
 *
 * Each rotator is two groups of registers 0x40 apart, one per NV12 plane -
 * rotator 0 at 0x040 and 0x080, rotator 1 at 0x0c0 and 0x100. The second group
 * is the chroma half and is given half the width and half the height, which is
 * what identifies it. Within a group:
 *
 *   +0x00  bits 10:0  source width, and bit 15 enables the engine
 *   +0x04  bits 10:0  source height
 *   +0x10  read address, low  (bits 19:4 of the address)
 *   +0x14  read address, high (bits 31:20)
 *   +0x18  pixel dummy
 *   +0x1c  write address, low
 *   +0x20  write address, high
 *
 * Addresses are encoded exactly as the MOP windows' are, MIU relative in sixteen
 * byte units. The enables and the direction are not in this page at all: rotator
 * 0's enable is bit 0 of 0x1f281028, and rotator 1's enable and both direction
 * bits are in 0x1f281144 - bit 0 rotator 1 enable, bit 1 rotator 0 direction,
 * bit 2 rotator 1 direction. Both of those are in the sub plane's page, and
 * 0x1f281144 is the one register mhal keeps a software shadow of, so it cannot be
 * read back and has to be written whole.
 *
 * Two things about them that decide how they can be used:
 *
 * They only rotate by a quarter turn. HalDispMopRot0SetRotateMode() takes 1 and
 * 3 and prints "Rotate ID %d not support" for 2, so there is no 180 degrees -
 * which is the one this panel would want, it being mounted upside down.
 *
 * They are not a memory to memory engine. Their write port only reaches the on
 * chip SRAM: pointed at an SRAM address the engine runs and the SRAM changes,
 * pointed at DRAM it runs and the DRAM is untouched. And the vendor only ever
 * gives them SRAM - _HalDispIfSetInputPortFlip() allocates height * 16 bytes of
 * luma, which is sixteen output lines rather than a frame, points the rotator's
 * write address at it and then points the MOP window's own read address at the
 * same place. So this is an in line stage of the display path that transposes a
 * window's source through a rolling strip of SRAM, and using it means a rotation
 * property on the MOP planes plus an allocation from the sram node, not a V4L2
 * mem2mem device.
 */
static const struct reg_sequence mstar_moprot_output_init[] = {
	{ 0x048, 0x0820 }, { 0x04c, 0xa01f }, { 0x058, 0x0801 },
	{ 0x088, 0x0820 }, { 0x08c, 0xa01f }, { 0x098, 0x0801 },
	{ 0x0c8, 0x0820 }, { 0x0cc, 0xa01f }, { 0x0d8, 0x0801 },
	{ 0x108, 0x0820 }, { 0x10c, 0xa01f }, { 0x118, 0x0801 },
	{ 0x004, 0x0001 }, { 0x004, 0x0003 },
};

static const struct regmap_config mstar_moprot_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
};

static int mstar_moprot_bind(struct device *dev, struct device *master,
			 void *data)
{
	return 0;
}

static void mstar_moprot_unbind(struct device *dev, struct device *master,
			    void *data)
{
}


static const struct component_ops mstar_moprot_component_ops = {
	.bind	= mstar_moprot_bind,
	.unbind	= mstar_moprot_unbind,
};

static int mstar_moprot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mstar_moprot *moprot;
	struct regmap *regmap;
	void __iomem *base;
	int ret;

	moprot = devm_kzalloc(dev, sizeof(*moprot), GFP_KERNEL);
	if (!moprot)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &mstar_moprot_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	moprot->regmap = regmap;

	ret = regmap_multi_reg_write(regmap, mstar_moprot_output_init,
				     ARRAY_SIZE(mstar_moprot_output_init));
	if (ret)
		return ret;

	dev_set_drvdata(dev, moprot);

	return component_add(&pdev->dev, &mstar_moprot_component_ops);
}

static void mstar_moprot_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mstar_moprot_component_ops);
}

/* the sleep reset clears the routing, so put it back before the mode set */
static int mstar_moprot_resume_noirq(struct device *dev)
{
	struct mstar_moprot *moprot = dev_get_drvdata(dev);

	return regmap_multi_reg_write(moprot->regmap, mstar_moprot_output_init,
				      ARRAY_SIZE(mstar_moprot_output_init));
}

static const struct dev_pm_ops mstar_moprot_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, mstar_moprot_resume_noirq)
};

static const struct of_device_id mstar_moprot_ids[] = {
	{
		.compatible = "sstar,ssd20xd-moprot",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mstar_moprot_ids);

static struct platform_driver mstar_moprot_driver = {
	.probe = mstar_moprot_probe,
	.remove = mstar_moprot_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_moprot_ids,
		.pm = pm_sleep_ptr(&mstar_moprot_pm_ops),
	},
};
module_platform_driver(mstar_moprot_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(DRIVER_NAME);
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
