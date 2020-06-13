#ifndef _DT_BINDINGS_GPIO_MSTAR_H
#define _DT_BINDINGS_GPIO_MSTAR_H

#define MSC313_FUART		0
#define MSC313_FUART_RX		(MSC313_FUART + 0)
#define MSC313_FUART_TX		(MSC313_FUART + 1)
#define MSC313_FUART_CTS	(MSC313_FUART + 2)
#define MSC313_FUART_RTS	(MSC313_FUART + 3)

#define MSC313_I2C1		(MSC313_FUART_RTS + 1)
#define MSC313_I2C1_SCL		(MSC313_I2C1 + 0)
#define MSC313_I2C1_SDA		(MSC313_I2C1 + 1)

#define MSC313_SR		(MSC313_I2C1_SDA + 1)
#define MSC313_SR_IO2		(MSC313_SR + 0)
#define MSC313_SR_IO3		(MSC313_SR + 1)
#define MSC313_SR_IO4		(MSC313_SR + 2)
#define MSC313_SR_IO5		(MSC313_SR + 3)
#define MSC313_SR_IO6		(MSC313_SR + 4)
#define MSC313_SR_IO7		(MSC313_SR + 5)
#define MSC313_SR_IO8		(MSC313_SR + 6)
#define MSC313_SR_IO9		(MSC313_SR + 7)
#define MSC313_SR_IO10		(MSC313_SR + 8)
#define MSC313_SR_IO11		(MSC313_SR + 9)
#define MSC313_SR_IO12		(MSC313_SR + 10)
#define MSC313_SR_IO13		(MSC313_SR + 11)
#define MSC313_SR_IO14		(MSC313_SR + 12)
#define MSC313_SR_IO15		(MSC313_SR + 13)
#define MSC313_SR_IO16		(MSC313_SR + 14)
#define MSC313_SR_IO17		(MSC313_SR + 15)

#define MSC313_PM_GPI04		0
#define MSC313_PM_SD_SDZ	1

//static unsigned msc313_offsets[] = {
//		0x50, /* FUART_RX	*/
//		0x54, /* FUART_TX	*/
//		0x58, /* FUART_CTS	*/
//		0x5c, /* FUART_RTS	*/
//		0x188, /* I2C1_SCL	*/
//		0x18c, /* I2C1_SDA	*/
//		0x88, /* SR_IO2		*/
//		0x8c, /* SR_IO3		*/
//		0x90, /* SR_IO4		*/
//		0x94, /* SR_IO5		*/
//		0x98, /* SR_IO6		*/
//		0x9c, /* SR_IO7		*/
//		0xa0, /* SR_IO8		*/
//		0xa4, /* SR_IO9		*/
//		0xa8, /* SR_IO10	*/
//		0xac, /* SR_IO11	*/
//		0xb0, /* SR_IO12	*/
//		0xb4, /* SR_IO13	*/
//		0xb8, /* SR_IO14	*/
//		0xbc, /* SR_IO15	*/
//		0xc0, /* SR_IO16	*/
//		0xc4, /* SR_IO17	*/
//		0x1c0, /* SPI0_CZ	*/
//		0x1c4, /* SPI0_CK	*/
//		0x1c8, /* SPI0_DI	*/
//		0x1cc, /* SPI0_DO	*/
//		0x140, /* SD_CLK	*/
//		0x144, /* SD_CMD	*/
//		0x148, /* SD_D0		*/
//		0x14c, /* SD_D1		*/
//		0x150, /* SD_D2		*/
//		0x154, /* SD_D3		*/
//};

#define SSC8336_UNKNOWN_0	0

/* SSC8336 FUART */
#define SSC8336_FUART		1
#define SSC8336_FUART_RX	SSC8336_FUART
#define SSC8333_FUART_TX	(SSC8336_FUART + 1)
#define SSC8336_FUART_CTS	(SSC8336_FUART + 2)
#define SSC8333_FUART_RTS	(SSC8336_FUART + 3)

/* SSC8336 SPI0 */

#define SSC8336_SPI0		11
#define SSC8336_SPI0_CZ		SSC8336_SPI0
#define SSC8336_SPI0_CK		(SSC8336_SPI0 + 1)
#define SSC8336_SPI0_DI		(SSC8336_SPI0 + 2)
#define SSC8336_SPI0_DO		(SSC8336_SPI0 + 3)

#define SSC8336_PM_GPIO0	0
#define SSC8336_PM_GPIO2	1
#define SSC8336_PM_GPIO4	2
#define SSC8336_PM_GPIO5	3
#define SSC8336_PM_GPIO6	4
#define SSC8336_PM_GPIO8	5
#define SSC8336_PM_SPI_DO	6
#define SSC8336_PM_SD_SDZ	7

#define PM_GPIO4_IRQ	6
#define PM_GPIO5_IRQ	7

#endif
