// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/regmap.h>

#include <dt-bindings/pinctrl/mstar.h>

#include "core.h"
#include "devicetree.h"
#include "pinconf.h"
#include "pinmux.h"

#define DRIVER_NAME			"pinctrl-mstar"

#define REG_UARTS		0xc
#define REG_PWMS		0x1c
#define REG_SDIO_NAND		0x20
#define REG_I2CS		0x24
#define REG_SPIS		0x30
#define REG_ETH_JTAG		0x3c
#define REG_SENSOR_CONFIG	0x54
#define REG_TX_MIPI_UART2	0x58
#define REG_SDIO_PULLDRIVE	0xc8

/* common group select registers and masks */
#define REG_FUART	REG_UARTS
#define MASK_FUART	(BIT(1) | BIT(0))
#define REG_UART0	REG_UARTS
#define MASK_UART0	(BIT(5) | BIT(4))
#define REG_UART1	REG_UARTS
#define MASK_UART1	(BIT(9) | BIT(8))

#define REG_PWM0	REG_PWMS
#define MASK_PWM0	(BIT(1) | BIT(0))
#define REG_PWM1	REG_PWMS
#define MASK_PWM1	(BIT(3) | BIT(2))
#define REG_PWM2	REG_PWMS
#define MASK_PWM2	(BIT(5) | BIT(4))
#define REG_PWM3	REG_PWMS
#define MASK_PWM3	(BIT(7) | BIT(6))
#define REG_PWM4	REG_PWMS
#define MASK_PWM4	(BIT(9) | BIT(8))
#define REG_PWM5	REG_PWMS
#define MASK_PWM5	(BIT(11)| BIT(10))
#define REG_PWM6	REG_PWMS
#define MASK_PWM6	(BIT(13)| BIT(11))
#define REG_PWM7	REG_PWMS
#define MASK_PWM7	(BIT(15)| BIT(14))

#define REG_SDIO	REG_SDIO_NAND
#define MASK_SDIO	BIT(8)

#define REG_I2C0	REG_I2CS
#define MASK_I2C0	(BIT(1) | BIT(0))
#define REG_I2C1	REG_I2CS
#define MASK_I2C1	(BIT(5) | BIT(4))

#define REG_SPI0	REG_SPIS
#define MASK_SPI0	(BIT(1) | BIT(0))
#define REG_SPI1	REG_SPIS
#define MASK_SPI1	(BIT(5) | BIT(4))

#define REG_JTAG	REG_ETH_JTAG
#define MASK_JTAG	BIT(1) | BIT(0)

#define REG_ETH		REG_ETH_JTAG
#define MASK_ETH	BIT(2)

#define REG_SR0_MIPI	REG_SENSOR_CONFIG
#define MASK_SR0_MIPI	(BIT(9) | BIT(8))

#define REG_SR1_BT656	REG_SENSOR_CONFIG
#define MASK_SR1_BT656	BIT(12)

#define REG_SR1_MIPI	REG_SENSOR_CONFIG
#define MASK_SR1_MIPI	(BIT(15) | BIT(14) | BIT(13))

#define REG_TX_MIPI	REG_TX_MIPI_UART2
#define MASK_TX_MIPI	(BIT(1) | BIT(0))

/* common pin group names */
#define GROUPNAME_PM_UART		"pm_uart"
#define GROUPNAME_PM_SPI		"pm_spi"
#define GROUPNAME_SD			"sd"
#define GROUPNAME_SD_D0_D1_D2_D3	"sd_d0_d1_d2_d3"
#define GROUPNAME_USB			"usb"
#define GROUPNAME_USB1			"usb1"
#define GROUPNAME_I2C0			"i2c0"
#define GROUPNAME_I2C1			"i2c1"
#define GROUPNAME_FUART			"fuart"
#define GROUPNAME_FUART_RX		"fuart_rx"
#define GROUPNAME_FUART_TX		"fuart_tx"
#define GROUPNAME_FUART_CTS		"fuart_cts"
#define GROUPNAME_FUART_RTS		"fuart_rts"
#define GROUPNAME_FUART_RX_TX		"fuart_rx_tx"
#define GROUPNAME_FUART_RX_TX_RTS	"fuart_rx_tx_rts"
#define GROUPNAME_FUART_CTS_RTS		"fuart_cts_rts"
#define GROUPNAME_FUART_CTS		"fuart_cts"
#define GROUPNAME_UART0			"uart0"
#define GROUPNAME_UART1			"uart1"
#define GROUPNAME_ETH			"eth"
#define GROUPNAME_PWM0			"pwm0"
#define GROUPNAME_PWM1			"pwm1"
#define GROUPNAME_PWM2			"pwm2"
#define GROUPNAME_PWM3			"pwm3"
#define GROUPNAME_PWM4			"pwm4"
#define GROUPNAME_PWM5			"pwm5"
#define GROUPNAME_PWM6			"pwm6"
#define GROUPNAME_PWM7			"pwm7"
#define GROUPNAME_SPI0			"spi0"
#define GROUPNAME_SPI0_CZ		"spi0_cz"
#define GROUPNAME_SPI0_CK		"spi0_ck"
#define GROUPNAME_SPI0_DI		"spi0_di"
#define GROUPNAME_SPI0_DO		"spi0_do"
#define GROUPNAME_SPI1			"spi1"

#define GROUPNAME_SR0_MIPI_MODE1	"sr0_mipi_mode1"
#define GROUPNAME_SR0_MIPI_MODE2	"sr0_mipi_mode2"
#define GROUPNAME_SR1_BT656		"sr1_bt656"
#define GROUPNAME_SR1_MIPI_MODE4	"sr1_mipi_mode4"

#define GROUPNAME_TX_MIPI_MODE1		"tx_mipi_mode1"
#define GROUPNAME_TX_MIPI_MODE2		"tx_mipi_mode2"

/* common group function names */
#define FUNCTIONNAME_PM_UART	GROUPNAME_PM_UART
#define FUNCTIONNAME_PM_SPI	GROUPNAME_PM_SPI
#define FUNCTIONNAME_USB	GROUPNAME_USB
#define FUNCTIONNAME_USB1	GROUPNAME_USB1
#define FUNCTIONNAME_FUART	GROUPNAME_FUART
#define FUNCTIONNAME_UART0	GROUPNAME_UART0
#define FUNCTIONNAME_UART1	GROUPNAME_UART1
#define FUNCTIONNAME_ETH	GROUPNAME_ETH
#define FUNCTIONNAME_JTAG	"jtag"
#define FUNCTIONNAME_PWM0	GROUPNAME_PWM0
#define FUNCTIONNAME_PWM1	GROUPNAME_PWM1
#define FUNCTIONNAME_PWM2	GROUPNAME_PWM2
#define FUNCTIONNAME_PWM3	GROUPNAME_PWM3
#define FUNCTIONNAME_PWM4	GROUPNAME_PWM4
#define FUNCTIONNAME_PWM5	GROUPNAME_PWM5
#define FUNCTIONNAME_PWM6	GROUPNAME_PWM6
#define FUNCTIONNAME_PWM7	GROUPNAME_PWM7
#define FUNCTIONNAME_SDIO	"sdio"
#define FUNCTIONNAME_I2C0	GROUPNAME_I2C0
#define FUNCTIONNAME_I2C1	GROUPNAME_I2C1
#define FUNCTIONNAME_SPI0	GROUPNAME_SPI0
#define FUNCTIONNAME_SPI1	GROUPNAME_SPI1

#define FUNCTIONNAME_SR0_MIPI	"sr0_mipi"
#define FUNCTIONNAME_SR1_BT656	GROUPNAME_SR1_BT656
#define FUNCTIONNAME_SR1_MIPI	"sr1_mipi"

#define FUNCTIONNAME_TX_MIPI	"tx_mipi"

/* common groups and register values.
 *  This maps functions to the groups that can handle
 *  a function and the register bits that need to be
 *  set to enable that function
 */
static const char* i2c0_groups[]    = { GROUPNAME_I2C0 };
static const u16   i2c0_values[]    = { BIT(0) };
static const char* i2c1_groups[]    = { GROUPNAME_I2C1 };
static const u16   i2c1_values[]    = { BIT(4)};
static const char* pm_uart_groups[] = { GROUPNAME_PM_UART };
static const char* fuart_groups[]   = { GROUPNAME_FUART, GROUPNAME_FUART_RX_TX_RTS };
static const u16   fuart_values[]   = { BIT(0), BIT(0) };
static const char* uart0_groups[]   = { GROUPNAME_FUART_RX_TX };
static const char* uart1_groups[]   = { GROUPNAME_FUART_CTS_RTS, GROUPNAME_FUART_CTS };
static const u16   uart1_values[]   = { BIT(9), BIT(9) };
static const char* pm_spi_groups[]  = { GROUPNAME_PM_SPI };
static const char* usb_groups[]     = { GROUPNAME_USB };
static const char* usb1_groups[]    = { GROUPNAME_USB1 };
static const char* pwm0_groups[]    = { GROUPNAME_FUART_RX };
static const u16   pwm0_values[]    = { BIT(1) | BIT(0) };
static const char* pwm1_groups[]    = { GROUPNAME_FUART_TX };
static const u16   pwm1_values[]    = { BIT(3) | BIT(2) };
static const char* pwm2_groups[]    = { GROUPNAME_FUART_CTS };
static const u16   pwm2_values[]    = { BIT(5) };
static const char* pwm3_groups[]    = { GROUPNAME_FUART_RTS };
static const u16   pwm3_values[]    = { BIT(7) };
static const char* pwm4_groups[]    = { GROUPNAME_SPI0_CZ };
static const u16   pwm4_values[]    = { BIT(9) };
static const char* pwm5_groups[]    = { GROUPNAME_SPI0_CK };
static const u16   pwm5_values[]    = { BIT(11) };
static const char* pwm6_groups[]    = { GROUPNAME_SPI0_DI };
static const u16   pwm6_values[]    = { BIT(13) };
static const char* pwm7_groups[]    = { GROUPNAME_SPI0_DO };
static const u16   pwm7_values[]    = { BIT(15) };
static const char* eth_groups[]     = { GROUPNAME_ETH };
static const u16   eth_values[]     = { BIT(2) };
static const char* jtag_groups[]    = { GROUPNAME_FUART };
static const char* spi0_groups[]    = { GROUPNAME_SPI0, GROUPNAME_FUART };
static const u16   spi0_values[]    = { BIT(0), BIT(1) | BIT(0) };
static const char* spi1_groups[]    = { GROUPNAME_SD_D0_D1_D2_D3 };
static const u16   spi1_values[]    = { BIT(5) | BIT(4) };
static const char* sdio_groups[]    = { GROUPNAME_SD };
static const u16   sdio_values[]    = { BIT(8) };

static const char* sr0_mipi_groups[] = { GROUPNAME_SR0_MIPI_MODE1, GROUPNAME_SR0_MIPI_MODE2};
static const u16   sr0_mipi_values[] = { BIT(8), BIT(9) };
static const char* sr1_bt656_groups[] = { GROUPNAME_SR1_BT656 };
static const u16   sr1_bt656_values[] = { BIT(12) };
static const char* sr1_mipi_groups[] = { GROUPNAME_SR1_MIPI_MODE4 };
static const u16   sr1_mipi_values[] = { BIT(15) };

static const char* tx_mipi_groups[] = { GROUPNAME_TX_MIPI_MODE1, GROUPNAME_TX_MIPI_MODE2 };
static const u16   tx_mipi_values[] = { BIT(0), BIT(1) };

#define COMMON_PIN(_model, _pinname) PINCTRL_PIN(PIN_##_model##_##_pinname, PINNAME_##_pinname)

#define MSTAR_PINCTRL_FUNCTION(n, r, m, g, v) \
		{ .name = n, .reg = r, .mask = m, .groups = g, .values = v, .numgroups = ARRAY_SIZE(g) }
#define MSTAR_PINCTRL_GROUP(n, p) { .name = n, .pins = p, .numpins = ARRAY_SIZE(p) }

struct mstar_pinctrl_function {
	const char* name;
	int reg;
	u16 mask;
	const char** groups;
	const u16* values;
	int numgroups;
};

struct mstar_pinctrl_group {
	const char* name;
	const int* pins;
	const int numpins;
};

struct mstar_configurable_pin {
	const int pin;
	const int pullupreg;
	const int pullupenbit;
	const int drivereg;
	const int drivebit;
	const unsigned *drivecurrents;
	const int ndrivecurrents;
};

struct mstar_pinctrl_info {
	const struct pinctrl_pin_desc *pins;
	const int npins;
	const struct mstar_pinctrl_group *groups;
	const int ngroups;
	const struct mstar_pinctrl_function *functions;
	const int nfunctions;
	const struct mstar_configurable_pin *confpin;
	const int nconfpins;
};

struct mstar_pinctrl {
	struct device *dev;
	struct pinctrl_desc desc;
	struct pinctrl_dev *pctl;
	void __iomem *mux;
	struct regmap *regmap;
	const struct mstar_pinctrl_info *info;
};

#define COMMON_FIXED_FUNCTION(_NAME,_name) MSTAR_PINCTRL_FUNCTION(FUNCTIONNAME_##_NAME, -1, 0, _name##_groups, NULL)
#define COMMON_FUNCTION(_NAME,_name) MSTAR_PINCTRL_FUNCTION(FUNCTIONNAME_##_NAME, REG_##_NAME, MASK_##_NAME, _name##_groups, _name##_values)
#define COMMON_FUNCTION_NULLVALUES(_NAME,_name) MSTAR_PINCTRL_FUNCTION(FUNCTIONNAME_##_NAME, REG_##_NAME, MASK_##_NAME, _name##_groups, NULL)

#define COMMON_FUNCTIONS COMMON_FIXED_FUNCTION(PM_UART, pm_uart), \
			 COMMON_FIXED_FUNCTION(PM_SPI, pm_spi), \
			 COMMON_FIXED_FUNCTION(USB, usb), \
			 COMMON_FUNCTION(FUART, fuart), \
			 COMMON_FUNCTION_NULLVALUES(UART0, uart0), \
			 COMMON_FUNCTION(UART1, uart1), \
			 COMMON_FUNCTION(PWM0, pwm0), \
			 COMMON_FUNCTION(PWM1, pwm1), \
			 COMMON_FUNCTION(PWM2, pwm2), \
			 COMMON_FUNCTION(PWM3, pwm3), \
			 COMMON_FUNCTION(PWM4, pwm4), \
			 COMMON_FUNCTION(PWM5, pwm5), \
			 COMMON_FUNCTION(PWM6, pwm6), \
			 COMMON_FUNCTION(PWM7, pwm7), \
			 COMMON_FUNCTION(SDIO, sdio), \
			 COMMON_FUNCTION(I2C0, i2c0), \
			 COMMON_FUNCTION(I2C1, i2c1), \
			 COMMON_FUNCTION(SPI0, spi0), \
			 COMMON_FUNCTION(SPI1, spi1), \
			 COMMON_FUNCTION_NULLVALUES(JTAG, jtag), \
			 COMMON_FUNCTION(ETH, eth)

#if CONFIG_MACH_INFINITY

#define MSC313_COMMON_PIN(_pinname) COMMON_PIN(MSC313, _pinname)

/* pinctrl pins */
static struct pinctrl_pin_desc msc313_pins[] = {
	MSC313_COMMON_PIN(PM_SD_CDZ),
	MSC313_COMMON_PIN(PM_IRIN),
	MSC313_COMMON_PIN(PM_UART_RX),
	MSC313_COMMON_PIN(PM_UART_TX),
	MSC313_COMMON_PIN(PM_GPIO4),
	MSC313_COMMON_PIN(PM_SPI_CZ),
	MSC313_COMMON_PIN(PM_SPI_DI),
	MSC313_COMMON_PIN(PM_SPI_WPZ),
	MSC313_COMMON_PIN(PM_SPI_DO),
	MSC313_COMMON_PIN(PM_SPI_CK),
	MSC313_COMMON_PIN(ETH_RN),
	MSC313_COMMON_PIN(ETH_RP),
	MSC313_COMMON_PIN(ETH_TN),
	MSC313_COMMON_PIN(ETH_TP),
	MSC313_COMMON_PIN(FUART_RX),
	MSC313_COMMON_PIN(FUART_TX),
	MSC313_COMMON_PIN(FUART_CTS),
	MSC313_COMMON_PIN(FUART_RTS),
	MSC313_COMMON_PIN(I2C1_SCL),
	MSC313_COMMON_PIN(I2C1_SDA),
	PINCTRL_PIN(PIN_MSC313_SR_IO2,	"sr_io2"),
	PINCTRL_PIN(PIN_MSC313_SR_IO3,	"sr_io3"),
	PINCTRL_PIN(PIN_MSC313_SR_IO4,	"sr_io4"),
	PINCTRL_PIN(PIN_MSC313_SR_IO5,	"sr_io5"),
	PINCTRL_PIN(PIN_MSC313_SR_IO6,	"sr_io6"),
	PINCTRL_PIN(PIN_MSC313_SR_IO7,	"sr_io7"),
	PINCTRL_PIN(PIN_MSC313_SR_IO8,	"sr_io8"),
	PINCTRL_PIN(PIN_MSC313_SR_IO9,	"sr_io9"),
	PINCTRL_PIN(PIN_MSC313_SR_IO10,	"sr_io10"),
	PINCTRL_PIN(PIN_MSC313_SR_IO11,	"sr_io11"),
	PINCTRL_PIN(PIN_MSC313_SR_IO12,	"sr_io12"),
	PINCTRL_PIN(PIN_MSC313_SR_IO13,	"sr_io13"),
	PINCTRL_PIN(PIN_MSC313_SR_IO14,	"sr_io14"),
	PINCTRL_PIN(PIN_MSC313_SR_IO15,	"sr_io15"),
	PINCTRL_PIN(PIN_MSC313_SR_IO16,	"sr_io16"),
	PINCTRL_PIN(PIN_MSC313_SR_IO17,	"sr_io17"),
	MSC313_COMMON_PIN(SPI0_CZ),
	MSC313_COMMON_PIN(SPI0_CK),
	MSC313_COMMON_PIN(SPI0_DI),
	MSC313_COMMON_PIN(SPI0_DO),
	MSC313_COMMON_PIN(SD_CLK),
	MSC313_COMMON_PIN(SD_CMD),
	MSC313_COMMON_PIN(SD_D0),
	MSC313_COMMON_PIN(SD_D1),
	MSC313_COMMON_PIN(SD_D2),
	MSC313_COMMON_PIN(SD_D3),
	MSC313_COMMON_PIN(USB_DM),
	MSC313_COMMON_PIN(USB_DP),
};

/* mux pin groupings */
static int msc313_pm_uart_pins[]         = { PIN_MSC313_PM_UART_RX, PIN_MSC313_PM_UART_TX };
static int msc313_pm_spi_pins[]          = { PIN_MSC313_PM_SPI_CZ, PIN_MSC313_PM_SPI_DI,
					     PIN_MSC313_PM_SPI_WPZ, PIN_MSC313_PM_SPI_DO,
					     PIN_MSC313_PM_SPI_CK };
static int msc313_eth_pins[]             = { PIN_MSC313_ETH_RN, PIN_MSC313_ETH_RP,
					     PIN_MSC313_ETH_TN, PIN_MSC313_ETH_TP };
static int msc313_fuart_pins[]           = { PIN_MSC313_FUART_RX, PIN_MSC313_FUART_TX,
					     PIN_MSC313_FUART_CTS, PIN_MSC313_FUART_RTS };
static int msc313_fuart_rx_pins[]        = { PIN_MSC313_FUART_RX };
static int msc313_fuart_tx_pins[]        = { PIN_MSC313_FUART_TX };
static int msc313_fuart_cts_pins[]       = { PIN_MSC313_FUART_CTS };
static int msc313_fuart_rts_pins[]       = { PIN_MSC313_FUART_RTS };
static int msc313_fuart_rx_tx_rts_pins[] = { PIN_MSC313_FUART_RX,
		                             PIN_MSC313_FUART_TX,
					     PIN_MSC313_FUART_RTS };
static int msc313_fuart_cts_rts_pins[]   = { PIN_MSC313_FUART_CTS, PIN_MSC313_FUART_RTS };
static int msc313_i2c1_pins[]            = { PIN_MSC313_I2C1_SCL, PIN_MSC313_I2C1_SDA };
static int msc313_spi0_pins[]            = { PIN_MSC313_SPI0_CZ,
					     PIN_MSC313_SPI0_CK,
					     PIN_MSC313_SPI0_DI,
					     PIN_MSC313_SPI0_DO };
static int msc313_spi0_cz_pins[]         = { PIN_MSC313_SPI0_CZ };
static int msc313_spi0_ck_pins[]         = { PIN_MSC313_SPI0_CK };
static int msc313_spi0_di_pins[]         = { PIN_MSC313_SPI0_DI };
static int msc313_spi0_do_pins[]         = { PIN_MSC313_SPI0_DO };
static int msc313_sd_d0_d1_d2_d3_pins[]  = { PIN_MSC313_SD_D0, PIN_MSC313_SD_D1,
					     PIN_MSC313_SD_D2,PIN_MSC313_SD_D3 };
static int msc313_sd_pins[]              = { PIN_MSC313_SD_CLK, PIN_MSC313_SD_CMD,
					     PIN_MSC313_SD_D0, PIN_MSC313_SD_D1,
					     PIN_MSC313_SD_D2, PIN_MSC313_SD_D3 };
static int msc313_usb_pins[]             = { PIN_MSC313_USB_DM, PIN_MSC313_USB_DP };


#define MSC313_PINCTRL_GROUP(_NAME, _name) MSTAR_PINCTRL_GROUP(GROUPNAME_##_NAME, msc313_##_name##_pins)

static const struct mstar_pinctrl_group msc313_pinctrl_groups[] = {
	MSC313_PINCTRL_GROUP(PM_UART, pm_uart),
	MSC313_PINCTRL_GROUP(PM_SPI, pm_spi),
	MSC313_PINCTRL_GROUP(USB, usb),
	MSC313_PINCTRL_GROUP(ETH, eth),
	MSC313_PINCTRL_GROUP(FUART, fuart),
	MSC313_PINCTRL_GROUP(FUART_RX, fuart_rx),
	MSC313_PINCTRL_GROUP(FUART_TX, fuart_tx),
	MSC313_PINCTRL_GROUP(FUART_CTS, fuart_cts),
	MSC313_PINCTRL_GROUP(FUART_RTS, fuart_rts),
	MSC313_PINCTRL_GROUP(FUART_RX_TX_RTS, fuart_rx_tx_rts),
	MSC313_PINCTRL_GROUP(FUART_CTS_RTS, fuart_cts_rts),
	MSC313_PINCTRL_GROUP(I2C1, i2c1),
	MSC313_PINCTRL_GROUP(SPI0, spi0),
	MSC313_PINCTRL_GROUP(SPI0_CZ, spi0_cz),
	MSC313_PINCTRL_GROUP(SPI0_CK, spi0_ck),
	MSC313_PINCTRL_GROUP(SPI0_DI, spi0_di),
	MSC313_PINCTRL_GROUP(SPI0_DO, spi0_do),
	MSC313_PINCTRL_GROUP(SD_D0_D1_D2_D3, sd_d0_d1_d2_d3),
	MSC313_PINCTRL_GROUP(SD, sd),
};

static const struct mstar_pinctrl_function msc313_pinctrl_functions[] = {
	COMMON_FUNCTIONS
};

#define MSTAR_PINCTRL_PIN(_pin, _pullupreg, _pullupenbit, _drivereg, _drivebit, _drivecurrents) \
	{ \
		.pin = _pin, \
		.pullupreg = _pullupreg, \
		.pullupenbit = _pullupenbit, \
		.drivereg = _drivereg, \
		.drivebit = _drivebit, \
		.drivecurrents = _drivecurrents, \
		.ndrivecurrents = ARRAY_SIZE(_drivecurrents) \
	}

static const unsigned sd_drivestrenghts[] = {4, 8};

static const struct mstar_configurable_pin msc313_configurable_pins[] = {
	MSTAR_PINCTRL_PIN(PIN_MSC313_SD_CMD, REG_SDIO_PULLDRIVE, 8, REG_SDIO_PULLDRIVE, 0, sd_drivestrenghts),
	MSTAR_PINCTRL_PIN(PIN_MSC313_SD_D0, REG_SDIO_PULLDRIVE, 9, REG_SDIO_PULLDRIVE, 1, sd_drivestrenghts),
	MSTAR_PINCTRL_PIN(PIN_MSC313_SD_D1, REG_SDIO_PULLDRIVE, 10, REG_SDIO_PULLDRIVE, 2, sd_drivestrenghts),
	MSTAR_PINCTRL_PIN(PIN_MSC313_SD_D2, REG_SDIO_PULLDRIVE, 11, REG_SDIO_PULLDRIVE, 3, sd_drivestrenghts),
	MSTAR_PINCTRL_PIN(PIN_MSC313_SD_D3, REG_SDIO_PULLDRIVE, 12, REG_SDIO_PULLDRIVE, 4, sd_drivestrenghts),
	MSTAR_PINCTRL_PIN(PIN_MSC313_SD_CLK, -1, -1, REG_SDIO_PULLDRIVE, 5, sd_drivestrenghts),
};

#define MSTAR_PINCTRL_INFO(_chip) static const struct mstar_pinctrl_info _chip##_info = { \
	.pins = _chip##_pins, \
	.npins = ARRAY_SIZE(_chip##_pins), \
	.groups = _chip##_pinctrl_groups, \
	.ngroups = ARRAY_SIZE(_chip##_pinctrl_groups), \
	.functions = _chip##_pinctrl_functions, \
	.nfunctions = ARRAY_SIZE(_chip##_pinctrl_functions), \
	.confpin = _chip##_configurable_pins, \
	.nconfpins = ARRAY_SIZE(_chip##_configurable_pins),\
}

MSTAR_PINCTRL_INFO(msc313);
#endif /* infinity */

#ifdef CONFIG_MACH_MERCURY
/* pinctrl pins */
#define SSC8336N_COMMON_PIN(_pinname) COMMON_PIN(SSC8336N, _pinname)

static struct pinctrl_pin_desc ssc8336n_pins[] = {
	SSC8336N_COMMON_PIN(USB_DM1),
	SSC8336N_COMMON_PIN(USB_DP1),
	SSC8336N_COMMON_PIN(USB_DM),
	SSC8336N_COMMON_PIN(USB_DP),
	SSC8336N_COMMON_PIN(USB_CID),
	SSC8336N_COMMON_PIN(PM_SPI_CZ),
	SSC8336N_COMMON_PIN(PM_SPI_DI),
	SSC8336N_COMMON_PIN(PM_SPI_WPZ),
	SSC8336N_COMMON_PIN(PM_SPI_DO),
	SSC8336N_COMMON_PIN(PM_SPI_CK),
	SSC8336N_COMMON_PIN(PM_SPI_HOLD),
	SSC8336N_COMMON_PIN(PM_GPIO8),
	SSC8336N_COMMON_PIN(PM_GPIO6),
	SSC8336N_COMMON_PIN(PM_GPIO5),
	SSC8336N_COMMON_PIN(PM_GPIO4),
	SSC8336N_COMMON_PIN(PM_GPIO2),
	SSC8336N_COMMON_PIN(PM_GPIO0),
	SSC8336N_COMMON_PIN(PM_UART_TX),
	SSC8336N_COMMON_PIN(PM_UART_RX),
	SSC8336N_COMMON_PIN(PM_IRIN),
	SSC8336N_COMMON_PIN(PM_SD_CDZ),
	SSC8336N_COMMON_PIN(FUART_RX),
	SSC8336N_COMMON_PIN(FUART_TX),
	SSC8336N_COMMON_PIN(FUART_CTS),
	SSC8336N_COMMON_PIN(FUART_RTS),
	SSC8336N_COMMON_PIN(SPI0_DO),
	SSC8336N_COMMON_PIN(SPI0_DI),
	SSC8336N_COMMON_PIN(SPI0_CK),
	SSC8336N_COMMON_PIN(SPI0_CZ),
	SSC8336N_COMMON_PIN(SPI0_CZ1),
	SSC8336N_COMMON_PIN(I2C0_SCL),
	SSC8336N_COMMON_PIN(I2C0_SDA),
	SSC8336N_COMMON_PIN(SD_D1),
	SSC8336N_COMMON_PIN(SD_D0),
	SSC8336N_COMMON_PIN(SD_CLK),
	SSC8336N_COMMON_PIN(SD_CMD),
	SSC8336N_COMMON_PIN(SD_D3),
	SSC8336N_COMMON_PIN(SD_D2),
	SSC8336N_COMMON_PIN(SR0_D2),
	SSC8336N_COMMON_PIN(SR0_D3),
	SSC8336N_COMMON_PIN(SR0_D4),
	SSC8336N_COMMON_PIN(SR0_D5),
	SSC8336N_COMMON_PIN(SR0_D6),
	SSC8336N_COMMON_PIN(SR0_D7),
	SSC8336N_COMMON_PIN(SR0_D8),
	SSC8336N_COMMON_PIN(SR0_D9),
	SSC8336N_COMMON_PIN(SR0_D10),
	SSC8336N_COMMON_PIN(SR0_D11),
	SSC8336N_COMMON_PIN(SR0_GPIO0),
	SSC8336N_COMMON_PIN(SR0_GPIO1),
	SSC8336N_COMMON_PIN(SR0_GPIO2),
	SSC8336N_COMMON_PIN(SR0_GPIO3),
	SSC8336N_COMMON_PIN(SR0_GPIO4),
	SSC8336N_COMMON_PIN(SR0_GPIO5),
	SSC8336N_COMMON_PIN(SR0_GPIO6),
	SSC8336N_COMMON_PIN(SR1_GPIO0),
	SSC8336N_COMMON_PIN(SR1_GPIO1),
	SSC8336N_COMMON_PIN(SR1_GPIO2),
	SSC8336N_COMMON_PIN(SR1_GPIO3),
	SSC8336N_COMMON_PIN(SR1_GPIO4),
	SSC8336N_COMMON_PIN(SR1_D0P),
	SSC8336N_COMMON_PIN(SR1_D0N),
	SSC8336N_COMMON_PIN(SR1_CKP),
	SSC8336N_COMMON_PIN(SR1_CKN),
	SSC8336N_COMMON_PIN(SR1_D1P),
	SSC8336N_COMMON_PIN(SR1_D1N),
	SSC8336N_COMMON_PIN(LCD_HSYNC),
	SSC8336N_COMMON_PIN(LCD_VSYNC),
	SSC8336N_COMMON_PIN(LCD_PCLK),
	SSC8336N_COMMON_PIN(LCD_DE),
	SSC8336N_COMMON_PIN(LCD_0),
	SSC8336N_COMMON_PIN(LCD_1),
	SSC8336N_COMMON_PIN(LCD_2),
	SSC8336N_COMMON_PIN(LCD_3),
	SSC8336N_COMMON_PIN(LCD_4),
	SSC8336N_COMMON_PIN(LCD_5),
	SSC8336N_COMMON_PIN(LCD_6),
	SSC8336N_COMMON_PIN(LCD_7),
	SSC8336N_COMMON_PIN(LCD_8),
	SSC8336N_COMMON_PIN(LCD_9),
	SSC8336N_COMMON_PIN(LCD_10),
	SSC8336N_COMMON_PIN(LCD_11),
	SSC8336N_COMMON_PIN(LCD_12),
	SSC8336N_COMMON_PIN(LCD_13),
	SSC8336N_COMMON_PIN(LCD_14),
	SSC8336N_COMMON_PIN(LCD_15),
};

/* mux pin groupings */
static int ssc8336n_pm_uart_pins[] = { PIN_SSC8336N_PM_UART_TX, PIN_SSC8336N_PM_UART_RX };
static int ssc8336n_pm_spi_pins[]  = { PIN_SSC8336N_PM_SPI_CZ, PIN_SSC8336N_PM_SPI_CZ,
				       PIN_SSC8336N_PM_SPI_DI, PIN_SSC8336N_PM_SPI_WPZ,
				       PIN_SSC8336N_PM_SPI_DO, PIN_SSC8336N_PM_SPI_CK,
				       PIN_SSC8336N_PM_SPI_HOLD };
static int ssc8336n_i2c0_pins[]    = { PIN_SSC8336N_I2C0_SCL, PIN_SSC8336N_I2C0_SDA };
static int ssc8336n_i2c1_pins[]    = { PIN_SSC8336N_SR0_GPIO0, PIN_SSC8336N_SR0_GPIO1 };
static int ssc8336n_usb_pins[]     = { PIN_SSC8336N_USB_DM, PIN_SSC8336N_USB_DP };
static int ssc8336n_usb1_pins[]    = { PIN_SSC8336N_USB_DM1, PIN_SSC8336N_USB_DP1 };
static int ssc8336n_sd_pins[]      = { PIN_SSC8336N_SD_CLK, PIN_SSC8336N_SD_CMD,
				       PIN_SSC8336N_SD_D0, PIN_SSC8336N_SD_D1,
				       PIN_SSC8336N_SD_D2, PIN_SSC8336N_SD_D3 };
static int ssc8336n_fuart_pins[]   = { PIN_SSC8336N_FUART_RX, PIN_SSC8336N_FUART_TX,
				       PIN_SSC8336N_FUART_CTS, PIN_SSC8336N_FUART_RTS };
static int ssc8336n_lcd_d0_to_d9_pins[] = {
				       PIN_SSC8336N_LCD_0, PIN_SSC8336N_LCD_1,
				       PIN_SSC8336N_LCD_2, PIN_SSC8336N_LCD_3,
				       PIN_SSC8336N_LCD_4, PIN_SSC8336N_LCD_5,
				       PIN_SSC8336N_LCD_6, PIN_SSC8336N_LCD_7,
				       PIN_SSC8336N_LCD_8, PIN_SSC8336N_LCD_9
				      };

static int ssc8336n_sr0_d2_to_d11_pins[] = {
					PIN_SSC8336N_SR0_D2, PIN_SSC8336N_SR0_D3,
					PIN_SSC8336N_SR0_D4, PIN_SSC8336N_SR0_D5,
					PIN_SSC8336N_SR0_D6, PIN_SSC8336N_SR0_D7,
					PIN_SSC8336N_SR0_D8, PIN_SSC8336N_SR0_D9,
					PIN_SSC8336N_SR0_D10,PIN_SSC8336N_SR0_D11,
					};

#define SR0_MIPI_COMMON	PIN_SSC8336N_SR0_GPIO2, \
			PIN_SSC8336N_SR0_GPIO3, \
			PIN_SSC8336N_SR0_GPIO4, \
			PIN_SSC8336N_SR0_D2, \
			PIN_SSC8336N_SR0_D3, \
			PIN_SSC8336N_SR0_D4, \
			PIN_SSC8336N_SR0_D5, \
			PIN_SSC8336N_SR0_D6, \
			PIN_SSC8336N_SR0_D7

static int ssc8336n_sr0_mipi_mode1_pins[] = {
					SR0_MIPI_COMMON
					};

static int ssc8336n_sr0_mipi_mode2_pins[] = {
					SR0_MIPI_COMMON,
					PIN_SSC8336N_SR0_D8,
					PIN_SSC8336N_SR0_D9,
					PIN_SSC8336N_SR0_D10,
					PIN_SSC8336N_SR0_D11,
					};

static int ssc8336n_sr1_bt656_pins[] = {
					PIN_SSC8336N_SR1_GPIO0,
					PIN_SSC8336N_SR1_GPIO1,
					PIN_SSC8336N_SR1_GPIO2,
					PIN_SSC8336N_SR1_GPIO3,
					PIN_SSC8336N_SR1_GPIO4,
					};
/* incomplete */
static int ssc8336n_sr1_mipi_mode4_pins[] = {
					PIN_SSC8336N_SR1_D0P,
					PIN_SSC8336N_SR1_D0N,
					PIN_SSC8336N_SR1_CKP,
					PIN_SSC8336N_SR1_CKN,
					PIN_SSC8336N_SR1_D1P,
					PIN_SSC8336N_SR1_D1N,
					};

#define TX_MIPI_COMMON_PINS	PIN_SSC8336N_LCD_0, \
				PIN_SSC8336N_LCD_1, \
				PIN_SSC8336N_LCD_2, \
				PIN_SSC8336N_LCD_3, \
				PIN_SSC8336N_LCD_4, \
				PIN_SSC8336N_LCD_5

static int ssc8336n_tx_mipi_mode1_pins[] = {
		TX_MIPI_COMMON_PINS
};

static int ssc8336n_tx_mipi_mode2_pins[] = {
		TX_MIPI_COMMON_PINS,
		PIN_SSC8336N_LCD_6,
		PIN_SSC8336N_LCD_7,
		PIN_SSC8336N_LCD_8,
		PIN_SSC8336N_LCD_9,
};

#define SSC8336N_PINCTRL_GROUP(_NAME, _name) MSTAR_PINCTRL_GROUP(GROUPNAME_##_NAME, ssc8336n_##_name##_pins)

/* pinctrl groups */

#define GROUPNAME_LCD_DO_TO_D9	"lcd_d0_to_d9"
#define GROUPNAME_SR0_D2_TO_D11	"sr0_d2_to_d11"

static const struct mstar_pinctrl_group ssc8336n_pinctrl_groups[] = {
	SSC8336N_PINCTRL_GROUP(PM_UART,pm_uart),
	SSC8336N_PINCTRL_GROUP(PM_SPI,pm_spi),
	SSC8336N_PINCTRL_GROUP(I2C0,i2c0),
	SSC8336N_PINCTRL_GROUP(I2C1,i2c1),
	SSC8336N_PINCTRL_GROUP(USB,usb),
	SSC8336N_PINCTRL_GROUP(USB1,usb1),
	SSC8336N_PINCTRL_GROUP(SD,sd),
	SSC8336N_PINCTRL_GROUP(FUART,fuart),
	SSC8336N_PINCTRL_GROUP(LCD_DO_TO_D9,lcd_d0_to_d9),
	SSC8336N_PINCTRL_GROUP(SR0_D2_TO_D11, sr0_d2_to_d11),
	SSC8336N_PINCTRL_GROUP(SR0_MIPI_MODE1, sr0_mipi_mode1),
	SSC8336N_PINCTRL_GROUP(SR0_MIPI_MODE2, sr0_mipi_mode2),
	SSC8336N_PINCTRL_GROUP(SR1_BT656, sr1_bt656),
	SSC8336N_PINCTRL_GROUP(SR1_MIPI_MODE4, sr1_mipi_mode4),
	SSC8336N_PINCTRL_GROUP(TX_MIPI_MODE1, tx_mipi_mode1),
	SSC8336N_PINCTRL_GROUP(TX_MIPI_MODE2, tx_mipi_mode2),
};

static const struct mstar_pinctrl_function ssc8336n_pinctrl_functions[] = {
	COMMON_FUNCTIONS,
	COMMON_FUNCTION(SR0_MIPI, sr0_mipi),
	COMMON_FUNCTION(SR1_BT656, sr1_bt656),
	COMMON_FUNCTION(SR1_MIPI, sr1_mipi),
	COMMON_FUNCTION(TX_MIPI, tx_mipi),
	COMMON_FIXED_FUNCTION(USB1, usb1)
};

static const struct mstar_pinctrl_info ssc8336n_info = {
	.pins = ssc8336n_pins,
	.npins = ARRAY_SIZE(ssc8336n_pins),
	.groups  = ssc8336n_pinctrl_groups,
	.ngroups = ARRAY_SIZE(ssc8336n_pinctrl_groups),
	.functions = ssc8336n_pinctrl_functions,
	.nfunctions = ARRAY_SIZE(ssc8336n_pinctrl_functions),
};
#endif /* mercury5 */

static int mstar_dt_node_to_map(struct pinctrl_dev *pctldev,
			       struct device_node *np,
			       struct pinctrl_map **map,
			       unsigned int *num_maps)
{
	return pinconf_generic_dt_node_to_map(pctldev, np,
						map, num_maps,
						PIN_MAP_TYPE_INVALID);
}

static void mstar_dt_free_map(struct pinctrl_dev *pctldev,
			     struct pinctrl_map *map, unsigned int num_maps)
{
	kfree(map);
}

static const struct pinctrl_ops mstar_pinctrl_ops = {
	.get_groups_count	= pinctrl_generic_get_group_count,
	.get_group_name		= pinctrl_generic_get_group_name,
	.get_group_pins		= pinctrl_generic_get_group_pins,
	.dt_node_to_map		= mstar_dt_node_to_map,
	.dt_free_map		= mstar_dt_free_map,
};

static int mstar_set_mux(struct pinctrl_dev *pctldev, unsigned int func,
			   unsigned int group)
{
	struct mstar_pinctrl *pinctrl = pctldev->driver_data;
	const char *grpname = pinctrl_generic_get_group_name(pctldev, group);
	struct function_desc *funcdesc = pinmux_generic_get_function(pctldev, func);
	struct mstar_pinctrl_function *function = funcdesc->data;
	int i, ret = 0;

	if(function != NULL){
		if(function->reg >= 0 && function->values != NULL){
			for(i = 0; i < function->numgroups; i++){
				if(strcmp(function->groups[i], grpname) == 0){
					dev_dbg(pinctrl->dev, "updating mux reg %x\n", (unsigned) function->reg);
					ret = regmap_update_bits(pinctrl->regmap, function->reg,
							function->mask, function->values[i]);
					if(ret)
						dev_dbg(pinctrl->dev, "failed to update register\n");
					break;
				}
			}
		}
		else {
			dev_dbg(pinctrl->dev, "reg or values not found\n");
		}
	}
	else {
		dev_info(pinctrl->dev, "missing function data\n");
	}

	return ret;
}

static const struct pinmux_ops mstar_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name   = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux             = mstar_set_mux,
	.strict              = true,
};

static int mstar_pinctrl_parse_groups(struct mstar_pinctrl *pinctrl){
	int i, ret;
	for(i = 0; i < pinctrl->info->ngroups; i++){
		const struct mstar_pinctrl_group *grp = &pinctrl->info->groups[i];
		ret = pinctrl_generic_add_group(pinctrl->pctl, grp->name,
				grp->pins, grp->numpins, NULL);
	}
	return ret;
}

static int mstar_pinctrl_parse_functions(struct mstar_pinctrl *pinctrl){
	int i, ret;
	for(i = 0; i < pinctrl->info->nfunctions; i++){
		const struct mstar_pinctrl_function *func =  &pinctrl->info->functions[i];

		// clear any existing value for the function
		if(func->reg >= 0){
			regmap_update_bits(pinctrl->regmap, func->reg,
					func->mask, 0);
		}

		ret = pinmux_generic_add_function(pinctrl->pctl, func->name,
							func->groups, func->numgroups,func);
		if(ret < 0){
			dev_err(pinctrl->dev, "failed to add function: %d", ret);
			goto out;
		}
	}
	out:
	return ret;
}

static const struct regmap_config msc313e_pinctrl_regmap_config = {
		.name = "msc313e-pinctrl",
		.reg_bits = 16,
		.val_bits = 16,
		.reg_stride = 4,
};

static int mstar_set_config(struct mstar_pinctrl *pinctrl, int pin, unsigned long config){
	enum pin_config_param param = pinconf_to_config_param(config);
	u32 arg = pinconf_to_config_argument(config);
	int i;
	unsigned int mask;
	const struct mstar_configurable_pin *confpin;
	dev_dbg(pinctrl->dev, "setting %d:%u on pin %d\n", (int)config,(unsigned)arg, pin);
	for(i = 0; i < pinctrl->info->nconfpins; i++){
		if(pinctrl->info->confpin[i].pin == pin){
			confpin = &pinctrl->info->confpin[i];
			switch(param){
			case PIN_CONFIG_BIAS_PULL_UP:
				if(confpin->pullupreg != -1){
					dev_dbg(pinctrl->dev, "setting pull up %d on pin %d\n", (int) arg, pin);
					mask = 1 << confpin->pullupenbit;
					regmap_update_bits(pinctrl->regmap, confpin->pullupreg, mask, arg ? mask : 0);
				}
				else
					dev_info(pinctrl->dev, "pullup reg/bit isn't known for pin %d\n", pin);
			default:
				break;
			}
			return 0;
		}
	}
	return 0;
}

static int mstar_pinctrl_get_config(struct mstar_pinctrl *pinctrl, int pin, unsigned long *config){
	int i;
	const struct mstar_configurable_pin *confpin;
	unsigned val;
	enum pin_config_param param = pinconf_to_config_param(*config);
	unsigned crntidx;

	/* we only support a limited range of conf options so filter the
	 * ones we do here
	 */

	switch(param){
		case PIN_CONFIG_BIAS_PULL_UP:
		case PIN_CONFIG_DRIVE_STRENGTH:
			break;
		default:
			goto out;
	}

	/* try to find the configuration register(s) for the pin */
	for(i = 0; i < pinctrl->info->nconfpins; i++){
		if(pinctrl->info->confpin[i].pin == pin){
			confpin = &pinctrl->info->confpin[i];
			switch(param){
				case PIN_CONFIG_BIAS_PULL_UP:
					if(confpin->pullupreg != -1){
						regmap_read(pinctrl->regmap, confpin->pullupreg, &val);
						return val & BIT(confpin->pullupenbit) ? 0 : -EINVAL;
					}
					else
						goto out;
				case PIN_CONFIG_DRIVE_STRENGTH:
					if(confpin->drivereg != -1){
						regmap_read(pinctrl->regmap, confpin->drivereg, &val);
						crntidx = (val & BIT(confpin->drivebit)) >> confpin->drivebit;
						*config = pinconf_to_config_packed(param, confpin->drivecurrents[crntidx]);
						return 0;
					}
					else
						goto out;
				default:
					goto out;
			}
		}
	}

out:
	return -ENOTSUPP;
}

int mstar_pin_config_get(struct pinctrl_dev *pctldev,
			       unsigned pin,
			       unsigned long *config){
	struct mstar_pinctrl *pinctrl = pctldev->driver_data;
	return mstar_pinctrl_get_config(pinctrl, pin, config);
}

int mstar_pin_config_set(struct pinctrl_dev *pctldev,
			       unsigned pin,
			       unsigned long *configs,
			       unsigned num_configs){
	int i;
	struct mstar_pinctrl *pinctrl = pctldev->driver_data;
	for(i = 0; i < num_configs; i++){
		mstar_set_config(pinctrl, pin, configs[i]);
	}
	return 0;
}

int mstar_pin_config_group_get(struct pinctrl_dev *pctldev,
				     unsigned selector,
				     unsigned long *config){
	return -ENOTSUPP;
}

int mstar_pin_config_group_set(struct pinctrl_dev *pctldev,
				     unsigned selector,
				     unsigned long *configs,
				     unsigned num_configs){
	struct mstar_pinctrl *pinctrl = pctldev->driver_data;
	struct group_desc *group = pinctrl_generic_get_group(pctldev, selector);
	int i, j, ret;
	for(i = 0; i < group->num_pins; i++){
		for(j = 0; j < num_configs; j++){
			ret = mstar_set_config(pinctrl, group->pins[i], configs[j]);
			if(ret)
				return ret;
		}
	}
	return 0;
}

static const struct pinconf_ops mstar_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = mstar_pin_config_get,
	.pin_config_set = mstar_pin_config_set,
	.pin_config_group_get = mstar_pin_config_group_get,
	.pin_config_group_set = mstar_pin_config_group_set,
};


static int mstar_pinctrl_probe(struct platform_device *pdev)
{
	int ret;
	struct mstar_pinctrl *pinctrl;
	struct resource *res;
	const struct mstar_pinctrl_info *match_data;

	match_data = of_device_get_match_data(&pdev->dev);
	if (!match_data)
		return -EINVAL;

	pinctrl = devm_kzalloc(&pdev->dev, sizeof(*pinctrl), GFP_KERNEL);
	if (!pinctrl)
		return -ENOMEM;

	platform_set_drvdata(pdev, pinctrl);

	pinctrl->dev = &pdev->dev;
	pinctrl->info = match_data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pinctrl->mux = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pinctrl->mux))
		return PTR_ERR(pinctrl->mux);

	pinctrl->regmap = devm_regmap_init_mmio(pinctrl->dev, pinctrl->mux,
			&msc313e_pinctrl_regmap_config);
	if(IS_ERR(pinctrl->regmap)){
		dev_err(pinctrl->dev, "failed to register regmap");
		return PTR_ERR(pinctrl->regmap);
	}

	pinctrl->desc.name = DRIVER_NAME;
	pinctrl->desc.pctlops = &mstar_pinctrl_ops;
	pinctrl->desc.pmxops = &mstar_pinmux_ops;
	pinctrl->desc.confops = &mstar_pinconf_ops;
	pinctrl->desc.owner = THIS_MODULE;
	pinctrl->desc.pins = pinctrl->info->pins;
	pinctrl->desc.npins = pinctrl->info->npins;

	ret = devm_pinctrl_register_and_init(pinctrl->dev, &pinctrl->desc,
					     pinctrl, &pinctrl->pctl);

	if (ret) {
		dev_err(pinctrl->dev, "failed to register pinctrl\n");
		return ret;
	}

	ret = mstar_pinctrl_parse_functions(pinctrl);
	ret = mstar_pinctrl_parse_groups(pinctrl);

	ret = pinctrl_enable(pinctrl->pctl);
	if (ret)
		dev_err(pinctrl->dev, "failed to enable pinctrl\n");

	return 0;
}

static const struct of_device_id mstar_pinctrl_of_match[] = {
#if CONFIG_MACH_INFINITY
	{
		.compatible	= "mstar,msc313-pinctrl",
		.data		= &msc313_info,
	},
	{
		.compatible	= "mstar,msc313e-pinctrl",
		.data		= &msc313_info,
	},
	{
		.compatible	= "sstar,ssd202-pinctrl",
		.data		= &msc313_info,
	},
#endif
#ifdef CONFIG_MACH_MERCURY
	{
		.compatible	= "mstar,ssc8336-pinctrl",
		.data		= &ssc8336n_info,
	},
	{
		.compatible	= "mstar,ssc8336n-pinctrl",
		.data		= &ssc8336n_info,
	},
#endif
	{ }
};
MODULE_DEVICE_TABLE(of, mstar_pinctrl_of_match);

static struct platform_driver mstar_pinctrl_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_pinctrl_of_match,
	},
	.probe = mstar_pinctrl_probe,
};

module_platform_driver(mstar_pinctrl_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("Pin controller driver for MStar SoCs");
MODULE_LICENSE("GPL v2");
