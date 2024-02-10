// SPDX-License-Identifier: GPL-2.0
/*
 *
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/io.h>

#define REG_DATA				0x0
#define REG_CONT				0x2
#define REG_CONT_BIT_COUNT_MASK	0xf
#define REG_CONT_POL			BIT(4)
#define REG_CONT_PHA			BIT(5)
#define REG_CONT_IRQEN			BIT(6)
#define REG_CONT_IRQ			BIT(7)
#define REG_CONT_XCH			BIT(8)
#define REG_CONT_ENABLE			BIT(9)
#define REG_CONT_DATARATE_MASK	0x7
#define REG_CONT_DATARATE_SHIFT	13

struct dragonball_spi {
	void __iomem *base;
	struct clk *clk;
	struct completion done;
};

static irqreturn_t dragonball_spi_irq(int irq, void *dev_id)
{
	struct dragonball_spi *spi = dev_id;
	u16 cont;

	cont = readw(spi->base + REG_CONT);
	writew((cont & ~REG_CONT_IRQ), spi->base + REG_CONT);
	complete(&spi->done);

	return IRQ_HANDLED;
}

static int dragonball_spi_transfer_one(struct spi_controller *host,
			struct spi_device *device, struct spi_transfer *t)
{
	struct dragonball_spi *spi = spi_controller_get_devdata(host);
	u16 cont = readw(spi->base + REG_CONT);
	/*
	 * The latency for interrupts is way too high, just smash the
	 * bit for now.
	 */
	bool poll = true;
	int i;

	/* Configure mode */
	cont &= ~(REG_CONT_PHA | REG_CONT_POL);
	if (device->mode & SPI_CPHA)
		cont |= REG_CONT_PHA;
	if (device->mode & SPI_CPOL)
		cont |= REG_CONT_POL;

	/* Setup 16 bits per transfer initially */
	cont &= ~REG_CONT_BIT_COUNT_MASK;
	cont |= (16 - 1);

	for (i = 0; i < t->len; i += 2) {
		u8 *outb = ((u8*)t->tx_buf) + i;
		u8 *inb = ((u8*)t->rx_buf) + i;
		u16 out = 0xffff;
		/* Can we do two bytes at once? */
		bool dotwo = (i + 2) <= t->len;

		/* Switch back to 8 bits if doing a single byte */
		if (!dotwo) {
			cont &= ~REG_CONT_BIT_COUNT_MASK;
			cont |= (8 - 1);
		}

		if (t->tx_buf)
			out = outb[0];
		if(dotwo) {
			out <<= 8;
			out |= outb[1];
		}

		if (poll)
			cont &= ~REG_CONT_IRQEN;
		else
			cont |= REG_CONT_IRQEN;

		writew(out, spi->base + REG_DATA);
		writew((cont | REG_CONT_XCH), spi->base + REG_CONT);

		if (poll) {
			do {
				cont = readw(spi->base + REG_CONT);
			} while ((cont & REG_CONT_XCH));
		}
		else {
			wait_for_completion(&spi->done);
			reinit_completion(&spi->done);
		}

		if (t->rx_buf) {
			u16 in = readw(spi->base + REG_DATA);
			if (dotwo) {
				inb[1] = in & 0xff;
				in >>= 8;
			}

			inb[0] = in & 0xff;
		}
	}

	return 0;
}

static int dragonball_spi_probe(struct platform_device *pdev)
{
	struct dragonball_spi *spi;
	struct spi_controller *host;
	int ret, irq;
	u16 cont;

	host = spi_alloc_host(&pdev->dev, sizeof(struct dragonball_spi));
	if (!host)
		return -ENOMEM;

	spi = spi_controller_get_devdata(host);
	init_completion(&spi->done);
	platform_set_drvdata(pdev, host);

	spi->base = devm_platform_ioremap_resource(pdev, 0);
	if (!spi->base) {
		ret = -ENOMEM;
		goto put_host;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto put_host;
	}

	/* Define our host */
	host->dev.of_node = pdev->dev.of_node;
	host->bus_num = pdev->id;
	host->mode_bits = SPI_CPHA | SPI_CPOL;
	host->bits_per_word_mask = SPI_BPW_MASK(8);
	host->use_gpio_descriptors = true;
	host->transfer_one = dragonball_spi_transfer_one;

	pdev->dev.dma_mask = NULL;

	cont = readw(spi->base + REG_CONT);
	cont |= REG_CONT_ENABLE;
	/* Clear clock divider */
	cont &= ~(REG_CONT_DATARATE_MASK << REG_CONT_DATARATE_SHIFT);
	/* Disable the interrupt */
	cont &= ~REG_CONT_IRQEN;
	writew(cont, spi->base + REG_CONT);

	/* Register for SPI Interrupt */
	ret = devm_request_irq(&pdev->dev, irq, dragonball_spi_irq, 0,
			dev_name(&pdev->dev), spi);
	if(ret)
		return ret;

	return devm_spi_register_controller(&pdev->dev, host);

put_host:
	spi_controller_put(host);

	return ret;
}

static void dragonball_spi_remove(struct platform_device *pdev)
{
	struct spi_controller *host = platform_get_drvdata(pdev);
	struct dragonball_spi *spi = spi_controller_get_devdata(host);
}

static const struct of_device_id dragonball_spi_of_match[] = {
	{ .compatible = "motorola,mc68ez328-spi", },
	{}
};
MODULE_DEVICE_TABLE(of, dragonball_spi_of_match);

static struct platform_driver dragonball_spi_driver = {
	.probe = dragonball_spi_probe,
	.remove_new = dragonball_spi_remove,
	.driver = {
		.name = "dragonball_spi",
		.of_match_table = dragonball_spi_of_match,
	},
};
module_platform_driver(dragonball_spi_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("DragonBall SPI driver");
MODULE_LICENSE("GPL");
