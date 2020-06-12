#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <asm-generic/io.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>

/*
 * 0x0	- ctrl
 *  8  |   4     |   3   |  1   |  0
 * int | capture | clear | trig |  ~en
 * 
 * 0x08 - max low
 * 0x0c - max high
 * 0x10 - counter low word
 * 0x14 - counter high word
 * 
 */

#define DRIVER_NAME "msc313e-timer"
#define REG_CTRL         0
#define REG_COUNTER_LOW  0x10
#define REG_COUNTER_HIGH 0x14

struct msc313e_timer {
	__iomem void* base;
	struct clk *clk;
	struct clocksource clksrc;
	struct regmap *regmap;
	struct regmap_field* noten;
};
#define to_msc313e_timer(ptr) container_of(ptr, struct msc313e_timer, clksrc)

static const struct of_device_id msc313e_timer_dt_ids[] = {
	{ .compatible = "mstar,msc313e-timer" },
	{},
};
MODULE_DEVICE_TABLE(of, msc313e_timer_dt_ids);

static const struct regmap_config msc313_timer_regmap_config = {
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4,
};

static struct reg_field noten_field = REG_FIELD(REG_CTRL, 0, 0);

static irqreturn_t msc313e_timer_irq(int irq, void *data)
{
	return IRQ_HANDLED;
}

static u64 msc313e_timer_read(struct clocksource *cs)
{
	struct msc313e_timer *timer = to_msc313e_timer(cs);
	return (ioread16(timer->base + REG_COUNTER_LOW) | (ioread16(timer->base + REG_COUNTER_HIGH) << 16)) & cs->mask;
}

static int msc313e_timer_enable(struct clocksource *cs){
	struct msc313e_timer *timer = to_msc313e_timer(cs);
	regmap_field_write(timer->noten, 0);
	return 0;
};

static void msc313e_timer_disable(struct clocksource *cs){
	struct msc313e_timer *timer = to_msc313e_timer(cs);
	regmap_field_write(timer->noten, 1);
}

static int msc313e_timer_probe(struct platform_device *pdev)
{
	int ret;
	struct msc313e_timer *timer;
	struct resource *res;
	int irq;

	timer = devm_kzalloc(&pdev->dev, sizeof(*timer), GFP_KERNEL);
	if (!timer)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	timer->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(timer->base))
		return PTR_ERR(timer->base);
	
	timer->regmap = devm_regmap_init_mmio(&pdev->dev, timer->base,
			&msc313_timer_regmap_config);
	if(IS_ERR(timer->regmap)){
		dev_err(&pdev->dev, "failed to register regmap");
		return PTR_ERR(timer->regmap);
	}
	timer->noten = devm_regmap_field_alloc(&pdev->dev, timer->regmap, noten_field);
	regmap_field_write(timer->noten, 1);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
		if (!irq)
			return -EINVAL;

	ret = devm_request_irq(&pdev->dev, irq, msc313e_timer_irq, IRQF_SHARED,
			dev_name(&pdev->dev), timer);
	if (ret)
		return ret;

	timer->clk = of_clk_get(pdev->dev.of_node, 0);
	clk_prepare_enable(timer->clk);
	
	timer->clksrc.name = platform_get_device_id(pdev)->name;
	timer->clksrc.rating = 200;
	timer->clksrc.read = msc313e_timer_read;
	timer->clksrc.mask = CLOCKSOURCE_MASK(32);
	timer->clksrc.flags = CLOCK_SOURCE_IS_CONTINUOUS;
	timer->clksrc.enable =  msc313e_timer_enable;
	timer->clksrc.disable =  msc313e_timer_disable;
	
	return clocksource_register_hz(&timer->clksrc, clk_get_rate(timer->clk));
}

static int msc313e_timer_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver msc313e_timer_driver = {
	.probe = msc313e_timer_probe,
	.remove = msc313e_timer_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = msc313e_timer_dt_ids,
	},
};

module_platform_driver(msc313e_timer_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mstar MSC313e PWM driver");
MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
