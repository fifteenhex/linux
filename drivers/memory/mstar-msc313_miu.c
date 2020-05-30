#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>

#define DRIVER_NAME "msc313-miu"

/*
 * MSC313 MIU (memory interface unit?) - multiport ddr controller
 *
 * The product brief for the msc313e that is available
 * doesn't detail any of the registers for this but it
 * seems to match the MIU in another MStar chip called
 * the MSB2521 that does have a leaked datasheet available.
 * That said I can't be 100% sure that all the bits in the
 * registers match what is actually in the msc313 so I'll
 * document anything that matches and not just paste the
 * whole lot here. TL;DR; there be gaps.
 *
 * 0x1f202000?
 * In the MSB2521 datasheet this is called MIU_ATOP, miu analog?
 *
 * 0x004 -
 *    15 - 8    |    7 - 0
 * dqs waveform | clock waveform
 * 0xaa - x8?   | 0xaa - x8?
 * 0xcc - x4?   | 0xcc - x4?
 *
 * 0x1f202400
 * In the MSB2521 datasheet this is called MIU_DIG, miu digital?
 * 0x000 - config0
 *         13         |      5           |
 * enter self refresh | auto refresh off |
 *
 * 0x004 - config1
 *   15   |   14   | 13    |   12   |   7 - 6  | 5 - 4   |   3 - 2     |   1 - 0
 * cko_en | adr_en | dq_en | cke_en | columns  | banks   | bus width   | dram type
 *        |        |       |        | 0x0 - 8  | 0x0 - 2 | 0x0 - 16bit | 0x0 - SDR
 *                                  | 0x1 - 9  | 0x1 - 4 | 0x1 - 32bit | 0x1 - DDR
 *                                  | 0x2 - 10 | 0x2 - 8 | 0x2 - 64bit | 0x2 - DDR2
 *        		            |          |         |             | 0x3 - DDR3
 *
 * 0x008 - config2
 * |   4 - 0
 * | rd timing
 * | 0xD
 *
 * 0x00c - config3
 *
 * 0x010 - config4
 *
 *    15   |    14    | 13 - 8 |    7 - 4  |   3 - 0
 * trp[4]  |  trcd[4] |  trs   |  trp[3:0] | trcd[3:0]
 *    0    |     0    |  0x1e  |  0x9      | 0x9
 *
 * 0x014 - config5
 * 13 - 8 | 7 - 4 | 3 - 0
 *   trc  | trtp  | trrd
 *
 * 0x018 - config6
 * 15 - 12 | 11 - 8 | 7 - 4 | 3 - 0
 *  trtw   |  twtr  |  twr  |  twl
 *
 * 0x01c - config7
 *
 *    15  | 14   | 12 | 11 - 8 | 7 - 0
 * twr[4] | tccd |  txp   | trfc
 *
 * The vendor suspend code writes 0xFFFF to all of these
 * but the first where it writes 0xFFFE instead
 * Presumably this is to stop requests happening while it
 * is putting the memory into low power mode.
 *
 * 0x08c - group 0 request mask
 * 0x0cc - group 1 request mask
 * 0x10c - group 2 request mask
 * 0x14c - group 3 request mask
 *
 * 0x180 - protection 0 start
 * 0x184 - protection 0 end
 * 0x188 - protection 1 start
 * 0x18c - protection 1 end
 * 0x190 - protection 2 start
 * 0x194 - protection 2 end
 * 0x198 - protection 3 start
 * 0x19c - protection 3 end
 * 0x1a0 - protection msb 0 - 3
 * 0x1a4 - protection en / ddr size
 * top bits ddr size?
 * bottom bits protect 0 - 3 en
 * 0x1b4 - protection fault address low
 * 0x1b8 - protection fault address high
 * 0x1bc - protection fault status
 * 14 - 8    | 7 - 5  | 4         | 1        | 0
 * client id | hit no | hit flag? | int mask | clr
 *
 * 0x1f202200
 * This isn't in the MSB2521 datasheet but it looks like a bunch
 * more group registers.
 *
 * The vendor pm code writes 0xFFFF these before messing with
 * the DDR right after the 4 group registers above. Hence my guess
 * is that these are a bunch of strapped on groups.
 *
 * 0x00c - group 4 request mask?
 * 0x04c - group 5 request mask?
 */

#define REG_ANA_DDFSET_L		0x60
#define REG_ANA_DDFSET_H		0x64

#define REG_CONFIG1			0x4
#define REG_CONFIG1_TYPE		(BIT(1) | BIT(0))
#define REG_CONFIG1_TYPE_SDR		0
#define REG_CONFIG1_TYPE_DDR		BIT(0)
#define REG_CONFIG1_TYPE_DDR2		BIT(1)
#define REG_CONFIG1_TYPE_DDR3		(BIT(1) | BIT(0))
#define REG_CONFIG1_BUSWIDTH		(BIT(3) | BIT(2))
#define REG_CONFIG1_BUSWIDTH_SHIFT	2
#define REG_CONFIG1_BANKS		(BIT(5) | BIT(4))
#define REG_CONFIG1_BANKS_SHIFT		4
#define REG_CONFIG1_COLS		(BIT(7) | BIT(6))
#define REG_CONFIG1_COLS_SHIFT		6

#define REG_CONFIG2			0x8
#define REG_CONFIG2_RD_TIMING		GENMASK(4,0)


#define REG_CONFIG3			0xc

#define REG_CONFIG4			0x10
#define REG_CONFIG4_TRCD		GENMASK(3,0)
#define REG_CONFIG4_TRP			GENMASK(7,4)
#define REG_CONFIG4_TRP_SHIFT		4
#define REG_CONFIG4_TRAS		GENMASK(13,8)
#define REG_CONFIG4_TRAS_SHIFT		8
#define REG_CONFIG4_TRCD_MSB		BIT(14)
#define REG_CONFIG4_TRP_MSB		BIT(15)

#define REG_CONFIG5			0x14
#define REG_CONFIG5_TRRD		GENMASK(3,0)
#define REG_CONFIG5_TRTP		GENMASK(7,4)
#define REG_CONFIG5_TRTP_SHIFT		4
#define REG_CONFIG5_TRC			GENMASK(13, 8)
#define REG_CONFIG5_TRC_SHIFT		8

struct msc313_miu {
	struct device *dev;
	struct regmap *analog;
	struct regmap *digital;
	struct clk *miuclk;
	struct regulator *ddrreg;

	/* ddr pll bits */
	char const*ddrpllparents[1];
	struct clk_hw clk_hw;
};

#define to_miu(_hw) container_of(_hw, struct msc313_miu, clk_hw)

static const struct of_device_id msc313_miu_dt_ids[] = {
	{ .compatible = "mstar,msc313-miu" },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_miu_dt_ids);

static const struct regmap_config msc313_miu_analog_regmap_config = {
		.name = "msc313-miu-analog",
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4,
};

static const struct regmap_config msc313_miu_digital_regmap_config = {
		.name = "msc313-miu-digital",
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4,
};

static const char *types[] = {"SDR", "DDR", "DDR2", "DDR3"};

static int msc313_miu_read_trcd(struct msc313_miu *miu){
	unsigned config4;
	int ret;
	ret = regmap_read(miu->digital, REG_CONFIG4, &config4);
	if(!ret){
		ret = (config4 & REG_CONFIG4_TRCD) +
				(config4 & REG_CONFIG4_TRCD_MSB ? 1 << 4 : 0);
	}
	return ret;
}

static void msc313_miu_write_trcd(struct msc313_miu *miu, unsigned val){
	regmap_update_bits(miu->digital, REG_CONFIG4, REG_CONFIG4_TRCD, val);
}

static int msc313_miu_read_trp(struct msc313_miu *miu){
	unsigned config4;
	int ret;
	ret = regmap_read(miu->digital, REG_CONFIG4, &config4);
	if(!ret){
		ret = ((config4 & REG_CONFIG4_TRP) >> REG_CONFIG4_TRP_SHIFT) +
				(config4 & REG_CONFIG4_TRP_MSB ? 1 << 4 : 0);
	}
	return ret;
}

static void msc313_miu_write_trp(struct msc313_miu *miu, unsigned val){
	regmap_update_bits(miu->digital, REG_CONFIG4, REG_CONFIG4_TRP,
			val << REG_CONFIG4_TRP_SHIFT);
}

static int msc313_miu_read_tras(struct msc313_miu *miu){
	unsigned config4;
	int ret;
	ret = regmap_read(miu->digital, REG_CONFIG4, &config4);
	if(!ret){
		ret = (config4 & REG_CONFIG4_TRAS) >> REG_CONFIG4_TRAS_SHIFT;
	}
	return ret;
}

static int msc313_miu_read_trrd(struct msc313_miu *miu){
	unsigned config4;
	int ret;
	ret = regmap_read(miu->digital, REG_CONFIG5, &config4);
	if(!ret){
		ret = (config4 & REG_CONFIG5_TRRD);
	}
	return ret;
}

static int msc313_miu_read_trtp(struct msc313_miu *miu){
	unsigned config4;
	int ret;
	ret = regmap_read(miu->digital, REG_CONFIG5, &config4);
	if(!ret){
		ret = (config4 & REG_CONFIG5_TRTP) >> REG_CONFIG5_TRTP_SHIFT;
	}
	return ret;
}

static int msc313_miu_read_trc(struct msc313_miu *miu){
	unsigned config4;
	int ret;
	ret = regmap_read(miu->digital, REG_CONFIG5, &config4);
	if(!ret){
		ret = (config4 & REG_CONFIG5_TRC) >> REG_CONFIG5_TRC_SHIFT;
	}
	return ret;
}

static unsigned long mstar_miu_ddrpll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	// no idea if this calculation is correct
	struct msc313_miu *miu = to_miu(hw);
	unsigned int tmp;
	u64 freq;
	u64 base = (((u64)parent_rate * 4 * 4) << 19);
	u32 ddfset;

	regmap_read(miu->analog, REG_ANA_DDFSET_H, &tmp);
	ddfset = (tmp & 0xff) << 16;
	regmap_read(miu->analog, REG_ANA_DDFSET_L, &tmp);
	ddfset |= tmp;

	freq = div_u64(base, ddfset);

	return freq;
}

static const struct clk_ops mstar_miu_ddrpll_ops = {
		.recalc_rate = mstar_miu_ddrpll_recalc_rate,
};

static int msc313_miu_ddrpll_probe(struct platform_device *pdev,
		struct msc313_miu *miu)
{
	struct clk* clk;
	struct clk_init_data *clk_init;

	clk_init = devm_kzalloc(&pdev->dev, sizeof(*clk_init), GFP_KERNEL);
	if(!clk_init)
		return -ENOMEM;

	miu->clk_hw.init = clk_init;
	of_property_read_string_index(pdev->dev.of_node,
			"clock-output-names", 0, &clk_init->name);
	clk_init->ops = &mstar_miu_ddrpll_ops;
	clk_init->flags = CLK_IS_CRITICAL;
	clk_init->num_parents = 1;
	// ffs, the ddr syn mux must come second.. there's no way to get a parent by name
	// or specify the field for the parents? whaaaa...
	miu->ddrpllparents[0] = of_clk_get_parent_name(pdev->dev.of_node, 1);
	clk_init->parent_names = miu->ddrpllparents;

	clk = clk_register(&pdev->dev, &miu->clk_hw);
	if(IS_ERR(clk)){
		return -ENOMEM;
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_simple_get, clk);
}

static int msc313_miu_probe(struct platform_device *pdev)
{
	struct msc313_miu *miu;
	struct resource *mem;
	__iomem void *base;
	unsigned int config1;

	int banks, cols, buswidth;
	int trcd, trp, tras, trrd, trtp, trc;

	u32 dtval;

	miu = devm_kzalloc(&pdev->dev, sizeof(*miu), GFP_KERNEL);
	if (!miu)
		return -ENOMEM;
	
	miu->dev = &pdev->dev;
	
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(base))
		return PTR_ERR(base);
	
	miu->analog = devm_regmap_init_mmio(&pdev->dev, base,
				&msc313_miu_analog_regmap_config);
	if(IS_ERR(miu->analog)){
		return PTR_ERR(miu->analog);
	}
	
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(base))
		return PTR_ERR(base);
		
	miu->digital = devm_regmap_init_mmio(&pdev->dev, base,
			&msc313_miu_digital_regmap_config);
	if(IS_ERR(miu->digital)){
		return PTR_ERR(miu->digital);
	}
	
	miu->miuclk = devm_clk_get(&pdev->dev, "miu");
	if (IS_ERR(miu->miuclk)) {
		return PTR_ERR(miu->miuclk);
	}

	miu->ddrreg = devm_regulator_get_optional(&pdev->dev, "ddr");
	if (IS_ERR(miu->ddrreg)){
		return PTR_ERR(miu->ddrreg);
	}

	clk_prepare_enable(miu->miuclk);

	regmap_read(miu->digital, REG_CONFIG1, &config1);

	banks = 2  << ((config1 & REG_CONFIG1_BANKS) >> REG_CONFIG1_BANKS_SHIFT);
	cols = 8 + ((config1 & REG_CONFIG1_COLS) >> REG_CONFIG1_COLS_SHIFT);
	buswidth = (((config1 & REG_CONFIG1_BUSWIDTH) >> REG_CONFIG1_BUSWIDTH_SHIFT) + 1) * 16;


	dev_info(&pdev->dev, "Memory type is %s, %d banks and %d columns, %d bit bus", types[config1 & REG_CONFIG1_TYPE],
			banks, cols, buswidth);

	trcd = msc313_miu_read_trcd(miu);
	trp = msc313_miu_read_trp(miu);
	tras = msc313_miu_read_tras(miu);
	trrd = msc313_miu_read_trrd(miu);
	trtp = msc313_miu_read_trtp(miu);
	trc = msc313_miu_read_trc(miu);

	dev_info(miu->dev, "trcd: %d, trp: %d, tras: %d, trrd: %d, trtp: %d, trc: %d",
			trcd, trp, tras, trrd, trtp, trc);

	if(!of_property_read_u32(pdev->dev.of_node, "mstar,rd-timing", &dtval)) {
		dev_info(&pdev->dev, "Setting read back data delay to %d", (int) dtval);
		//regmap_update_bits(miu->digital, REG_CONFIG2, REG_CONFIG2_RD_TIMING, rd_timing);
	}

	if(!of_property_read_u32(pdev->dev.of_node, "mstar,trcd", &dtval)) {
		dev_info(&pdev->dev, "setting trcd to %d", (int) dtval);
		msc313_miu_write_trcd(miu, dtval);
	}

	if(!of_property_read_u32(pdev->dev.of_node, "mstar,trp", &dtval)) {
		dev_info(&pdev->dev, "setting trp to %d", (int) dtval);
		msc313_miu_write_trp(miu, dtval);
	}

	return msc313_miu_ddrpll_probe(pdev, miu);
}

static int msc313_miu_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver msc313_miu_driver = {
	.probe = msc313_miu_probe,
	.remove = msc313_miu_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = msc313_miu_dt_ids,
	},
};

module_platform_driver(msc313_miu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mstar MSC313 MIU driver");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
