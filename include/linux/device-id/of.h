/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_DEVICE_ID_OF_H
#define LINUX_DEVICE_ID_OF_H

/*
 * Struct used for matching a device
 */
struct of_device_id {
#ifdef CONFIG_OF_NAME_TYPE
	char name[32];
	char type[32];
#endif
	char compatible[CONFIG_OF_COMPATIBLE_MAX_LEN];
	const void *data;
};

#endif /* ifndef LINUX_DEVICE_ID_OF_H */
