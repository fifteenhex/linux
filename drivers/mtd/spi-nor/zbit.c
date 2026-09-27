// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021, Daniel Palmer<daniel@thingy.jp>
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info zbit_parts[] = {
	/* zbit */
	{
		.id = SNOR_ID(0x5e, 0x40, 0x18),
		.name = "zb25vq128",
		.size = SZ_16M,
		.flags = SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	},
};

const struct spi_nor_manufacturer spi_nor_zbit = {
	.name = "zbit",
	.parts = zbit_parts,
	.nparts = ARRAY_SIZE(zbit_parts),
};
