/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GPIO definitions for MStar/SigmaStar MSC313 and later SoCs
 *
 * Copyright (C) 2020 Daniel Palmer <daniel@thingy.jp>
 */

#ifndef _DT_BINDINGS_MSC313_GPIO_H
#define _DT_BINDINGS_MSC313_GPIO_H

/* pin names for fuart, same for all SoCs so far */
#define MSC313_PINNAME_FUART_RX		"fuart_rx"
#define MSC313_PINNAME_FUART_TX		"fuart_tx"
#define MSC313_PINNAME_FUART_CTS	"fuart_cts"
#define MSC313_PINNAME_FUART_RTS	"fuart_rts"

/* pin names for sr, mercury5 is different */
#define MSC313_PINNAME_SR_IO2		"sr_io2"
#define MSC313_PINNAME_SR_IO3		"sr_io3"
#define MSC313_PINNAME_SR_IO4		"sr_io4"
#define MSC313_PINNAME_SR_IO5		"sr_io5"
#define MSC313_PINNAME_SR_IO6		"sr_io6"
#define MSC313_PINNAME_SR_IO7		"sr_io7"
#define MSC313_PINNAME_SR_IO8		"sr_io8"
#define MSC313_PINNAME_SR_IO9		"sr_io9"
#define MSC313_PINNAME_SR_IO10		"sr_io10"
#define MSC313_PINNAME_SR_IO11		"sr_io11"
#define MSC313_PINNAME_SR_IO12		"sr_io12"
#define MSC313_PINNAME_SR_IO13		"sr_io13"
#define MSC313_PINNAME_SR_IO14		"sr_io14"
#define MSC313_PINNAME_SR_IO15		"sr_io15"
#define MSC313_PINNAME_SR_IO16		"sr_io16"
#define MSC313_PINNAME_SR_IO17		"sr_io17"

/* pin names for sd, same for all SoCs so far */
#define MSC313_PINNAME_SD_CLK		"sd_clk"
#define MSC313_PINNAME_SD_CMD		"sd_cmd"
#define MSC313_PINNAME_SD_D0		"sd_d0"
#define MSC313_PINNAME_SD_D1		"sd_d1"
#define MSC313_PINNAME_SD_D2		"sd_d2"
#define MSC313_PINNAME_SD_D3		"sd_d3"

/* pin names for i2c1, same for all SoCs so for */
#define MSC313_PINNAME_I2C1_SCL		"i2c1_scl"
#define MSC313_PINNAME_I2C1_SCA		"i2c1_sda"

/* pin names for spi0, same for all SoCs so far */
#define MSC313_PINNAME_SPI0_CZ		"spi0_cz"
#define MSC313_PINNAME_SPI0_CK		"spi0_ck"
#define MSC313_PINNAME_SPI0_DI		"spi0_di"
#define MSC313_PINNAME_SPI0_DO		"spi0_do"

#define MSC313_GPIO_FUART	0
#define MSC313_GPIO_FUART_RX	(MSC313_GPIO_FUART + 0)
#define MSC313_GPIO_FUART_TX	(MSC313_GPIO_FUART + 1)
#define MSC313_GPIO_FUART_CTS	(MSC313_GPIO_FUART + 2)
#define MSC313_GPIO_FUART_RTS	(MSC313_GPIO_FUART + 3)

#define MSC313_GPIO_SR		(MSC313_GPIO_FUART_RTS + 1)
#define MSC313_GPIO_SR_IO2	(MSC313_GPIO_SR + 0)
#define MSC313_GPIO_SR_IO3	(MSC313_GPIO_SR + 1)
#define MSC313_GPIO_SR_IO4	(MSC313_GPIO_SR + 2)
#define MSC313_GPIO_SR_IO5	(MSC313_GPIO_SR + 3)
#define MSC313_GPIO_SR_IO6	(MSC313_GPIO_SR + 4)
#define MSC313_GPIO_SR_IO7	(MSC313_GPIO_SR + 5)
#define MSC313_GPIO_SR_IO8	(MSC313_GPIO_SR + 6)
#define MSC313_GPIO_SR_IO9	(MSC313_GPIO_SR + 7)
#define MSC313_GPIO_SR_IO10	(MSC313_GPIO_SR + 8)
#define MSC313_GPIO_SR_IO11	(MSC313_GPIO_SR + 9)
#define MSC313_GPIO_SR_IO12	(MSC313_GPIO_SR + 10)
#define MSC313_GPIO_SR_IO13	(MSC313_GPIO_SR + 11)
#define MSC313_GPIO_SR_IO14	(MSC313_GPIO_SR + 12)
#define MSC313_GPIO_SR_IO15	(MSC313_GPIO_SR + 13)
#define MSC313_GPIO_SR_IO16	(MSC313_GPIO_SR + 14)
#define MSC313_GPIO_SR_IO17	(MSC313_GPIO_SR + 15)

#define MSC313_GPIO_SD		(MSC313_GPIO_SR_IO17 + 1)
#define MSC313_GPIO_SD_CLK	(MSC313_GPIO_SD + 0)
#define MSC313_GPIO_SD_CMD	(MSC313_GPIO_SD + 1)
#define MSC313_GPIO_SD_D0	(MSC313_GPIO_SD + 2)
#define MSC313_GPIO_SD_D1	(MSC313_GPIO_SD + 3)
#define MSC313_GPIO_SD_D2	(MSC313_GPIO_SD + 4)
#define MSC313_GPIO_SD_D3	(MSC313_GPIO_SD + 5)

#define MSC313_GPIO_I2C1	(MSC313_GPIO_SD_D3 + 1)
#define MSC313_GPIO_I2C1_SCL	(MSC313_GPIO_I2C1 + 0)
#define MSC313_GPIO_I2C1_SDA	(MSC313_GPIO_I2C1 + 1)

#define MSC313_GPIO_SPI0	(MSC313_GPIO_I2C1_SDA + 1)
#define MSC313_GPIO_SPI0_CZ	(MSC313_GPIO_SPI0 + 0)
#define MSC313_GPIO_SPI0_CK	(MSC313_GPIO_SPI0 + 1)
#define MSC313_GPIO_SPI0_DI	(MSC313_GPIO_SPI0 + 2)
#define MSC313_GPIO_SPI0_DO	(MSC313_GPIO_SPI0 + 3)

#endif /* _DT_BINDINGS_MSC313_GPIO_H */
