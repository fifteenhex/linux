#ifndef _DT_BINDINGS_GPIO_MSTAR_H
#define _DT_BINDINGS_GPIO_MSTAR_H

#define MSC313_FUART_RX		0
#define MSC313_FUART_TX		1

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
#define SSC8333_UNKNOWN_1	1
#define SSC8336_FUART_RX	2
#define SSC8333_FUART_TX	3


#define SSC8336_PM_GPIO0	0
#define SSC8336_PM_GPIO2	1
#define SSC8336_PM_GPIO4	2
#define SSC8336_PM_GPIO5	3
#define SSC8336_PM_GPIO6	4
#define SSC8336_PM_GPIO8	5
#define SSC8336_PM_SPI_DO	6
#define SSC8336_PM_SD_SDZ	7

#endif
