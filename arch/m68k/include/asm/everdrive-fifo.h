/* SPDX-License-Identifier: GPL-2.0 */
/*
 */

#ifndef _EVERDRIVE_FIFO_H
#define _EVERDRIVE_FIFO_H

#include <asm/io.h>
#include <linux/iopoll.h>

#define EVERDRIVE_FIFO_CMD_PREAMBLE	'+'
#define EVERDRIVE_FIFO_CMD_STATUS	0x10U
#define EVERDRIVE_FIFO_CMD_USB_WRITE	0x22U
#define EVERDRIVE_FIFO_CMD_FIFO_WRITE	0x23U

/* Disk commands */
#define EVERDRIVE_FIFO_CMD_DISK_INIT	0xC0U
#define EVERDRIVE_FIFO_CMD_DISK_RD	0xC1U
#define EVERDRIVE_FIFO_CMD_DISK_WR	0xC2U
#define EVERDRIVE_FIFO_CMD_F_DIR_OPN	0xC3U
#define EVERDRIVE_FIFO_CMD_F_DIR_RD	0xC4U
#define EVERDRIVE_FIFO_CMD_F_DIR_LD	0xC5U
#define EVERDRIVE_FIFO_CMD_F_DIR_SIZE	0xC6U
#define EVERDRIVE_FIFO_CMD_F_DIR_PATH	0xC7U
#define EVERDRIVE_FIFO_CMD_F_DIR_GET	0xC8U
#define EVERDRIVE_FIFO_CMD_F_FOPN	0xC9U
#define EVERDRIVE_FIFO_CMD_F_FRD	0xCAU
#define EVERDRIVE_FIFO_CMD_F_FRD_MEM	0xCBU
#define EVERDRIVE_FIFO_CMD_F_FWR	0xCCU
#define EVERDRIVE_FIFO_CMD_F_FWR_MEM	0xCDU
#define EVERDRIVE_FIFO_CMD_F_FCLOSE	0xCEU
#define EVERDRIVE_FIFO_CMD_F_FPTR	0xCFU
#define EVERDRIVE_FIFO_CMD_F_FINFO	0xD0U
#define EVERDRIVE_FIFO_CMD_F_FCRC	0xD1U
#define EVERDRIVE_FIFO_CMD_F_DIR_MK	0xD2U
#define EVERDRIVE_FIFO_CMD_F_DEL	0xD3U
#define EVERDRIVE_FIFO_CMD_F_SEEK_IDX	0xD4U
#define EVERDRIVE_FIFO_CMD_F_AVB	0xD5U
#define EVERDRIVE_FIFO_CMD_F_FCP	0xD6U
#define EVERDRIVE_FIFO_CMD_F_SEEK_PAT	0xD8U
#define EVERDRIVE_FIFO_CMD_F_DTEST	0xD9U
#define EVERDRIVE_FIFO_CMD_F_FTEST	0xDAU

extern struct mutex everdrive_fifo_mutex;

struct __attribute__((packed)) everdrive_fifo_cmd {
	uint8_t preamble;
	uint8_t _preable;
	uint8_t cmd;
	uint8_t _cmd;
};

#define EVERDRIVE_FIFO_DEFINECMD(_cmd) { \
		(unsigned char) EVERDRIVE_FIFO_CMD_PREAMBLE, (unsigned char) ~EVERDRIVE_FIFO_CMD_PREAMBLE, \
	  	(unsigned char) _cmd, (unsigned char) ~_cmd \
	};

static const struct everdrive_fifo_cmd cmd_status =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_STATUS);
static const struct everdrive_fifo_cmd cmd_usbwr =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_USB_WRITE);

/* Disk */
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_init =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_DISK_INIT);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_dir_ld =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_DIR_LD);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_dir_size =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_DIR_SIZE);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_dir_get =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_DIR_GET);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_fopen =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_FOPN);

#define	EVERDRIVE_FILE_MODE_READ		0x01
#define	EVERDRIVE_FILE_MODE_WRITE		0x02
#define	EVERDRIVE_FILE_MODE_OPEN_EXISTING	0x00
#define	EVERDRIVE_FILE_MODE_CREATE_NEW		0x04
#define	EVERDRIVE_FILE_MODE_CREATE_ALWAYS	0x08
#define	EVERDRIVE_FILE_MODE_OPEN_ALWAYS		0x10
#define	EVERDRIVE_FILE_MODE_OPEN_APPEND		0x30

static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_frd =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_FRD);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_fclose =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_FCLOSE);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_fptr =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_FPTR);
static const struct everdrive_fifo_cmd everdrive_fifo_cmd_disk_f_avb =
		EVERDRIVE_FIFO_DEFINECMD(EVERDRIVE_FIFO_CMD_F_AVB);

#define FIFO_CPU_RXF BIT(15)
#define FIFO_RXF_MSK 0x7FF

static __always_inline void everdrive_fifo_write(volatile void *fifo, const u8 *src, unsigned int len)
{
	/* For u64 */
	while (len >= 8) {
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);

		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);

		len -= 8;
	}

	/* For u32 */
	while (len >= 4) {
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		len -= 4;
	}

	/* For u16 */
	while (len >= 2) {
		iowrite16be(*src++, fifo);
		iowrite16be(*src++, fifo);
		len -= 2;
	}

	while (len) {
		iowrite16be(*src++, fifo);
		len--;
	}
}

static __always_inline void everdrive_fifo_write_u8(volatile void *fifo, u8 value)
{
	uint8_t tmp = value;

	everdrive_fifo_write(fifo, (void *) &tmp, sizeof(tmp));
}

static __always_inline void everdrive_fifo_write_u16(volatile void *fifo, u16 value)
{
	uint16_t tmp = value;

	everdrive_fifo_write(fifo, (void *) &tmp, sizeof(tmp));
}

static __always_inline void everdrive_fifo_write_u32(volatile void *fifo, u32 value)
{
	uint32_t tmp = value;

	everdrive_fifo_write(fifo, (void *) &tmp, sizeof(tmp));
}

static inline void everdrive_fifo_write_str(volatile void *fifo, const unsigned char *str, size_t len)
{
	everdrive_fifo_write_u16(fifo, len);
	everdrive_fifo_write(fifo, (void *) str, len);
}

static __always_inline bool everdrive_can_read(volatile void *fifo)
{
//	return (ioread16be(fifo + 2) & FIFO_CPU_RXF);
	return (ioread16be(fifo + 2) & FIFO_RXF_MSK);
}

static __always_inline u16 everdrive_fifo_level(volatile void *fifo)
{
	return (ioread16be(fifo + 2) & FIFO_RXF_MSK);
}

static __always_inline void everdrive_drain(volatile void *fifo)
{
	while (everdrive_can_read(fifo))
		ioread16be(fifo);
}

#define BLOCKOFOUR \
		*dst++ = (u8) (ioread16be(fifo) & 0xff); \
		*dst++ = (u8) (ioread16be(fifo) & 0xff); \
		*dst++ = (u8) (ioread16be(fifo) & 0xff); \
		*dst++ = (u8) (ioread16be(fifo) & 0xff);

/* Make sure you read the level first and know how much you can read! */
static __always_inline void _everdrive_fifo_read(volatile void *fifo, volatile u8 *dst, unsigned int len)
{
	for (; len >= 16; len -= 16) {
		BLOCKOFOUR
		BLOCKOFOUR
		BLOCKOFOUR
		BLOCKOFOUR
	}

	for (; len >= 8; len -= 8) {
		BLOCKOFOUR
		BLOCKOFOUR
	}

	for (; len >= 4; len -= 4) {
		BLOCKOFOUR
	}

	for (; len >= 2; len -= 2) {
		*dst++ = (u8) (ioread16be(fifo) & 0xff);
		*dst++ = (u8) (ioread16be(fifo) & 0xff);
	}

	for (; len; len--)
		*dst++ = (u8) (ioread16be(fifo) & 0xff);
}

static __always_inline int everdrive_fifo_read(volatile void *fifo, volatile u8 *dst, unsigned int len)
{
	while (len) {
		bool can_read;
		int readsz;
		int ret;

		ret = poll_timeout_us(can_read = everdrive_can_read(fifo), can_read, 1000000, 2000, false);
		if (ret)
			return ret;

		readsz = min(everdrive_fifo_level(fifo), len);

		_everdrive_fifo_read(fifo, dst, readsz);
		dst += readsz;
		len -= readsz;
	}

	return 0;
}

/* This waits for an exact amount to be readable in the fifo and then reads it
 * since the amount we need to read is known in advance the compiler can
 * optimize this for us
 */
static __always_inline int everdrive_fifo_read_exactly(volatile void *fifo, volatile u8 *dst, unsigned int len)
{
	unsigned int level = 0;
	int ret;

	ret = poll_timeout_us(level = everdrive_fifo_level(fifo), (level >= len), 1000000, 2000, false);
	if (ret)
		return ret;

	_everdrive_fifo_read(fifo, dst, len);

	return 0;
}

static __always_inline int everdrive_fifo_read_u8(volatile void *fifo, uint8_t *value)
{
	uint8_t tmp;
	int ret;

	ret = everdrive_fifo_read_exactly(fifo, (void *) &tmp, sizeof(tmp));
	if (ret)
		return ret;

	*value = tmp;

	return 0;
}

static __always_inline int everdrive_fifo_read_u16(volatile void *fifo, uint16_t *value)
{
	uint16_t tmp;
	int ret;

	ret = everdrive_fifo_read_exactly(fifo, (void *) &tmp, sizeof(tmp));
	if (ret)
		return ret;

	*value = tmp;

	return 0;
}

static __always_inline int everdrive_fifo_read_u32(volatile void *fifo, uint32_t *value)
{
	uint32_t tmp;
	int ret;

	ret = everdrive_fifo_read_exactly(fifo, (void *) &tmp, sizeof(tmp));
	if (ret)
		return ret;

	*value = tmp;

	return 0;
}

static inline int everdrive_fifo_read_u64(volatile void *fifo, uint64_t *value)
{
	volatile uint64_t tmp;
	int ret;

	ret = everdrive_fifo_read_exactly(fifo, (void *) &tmp, sizeof(tmp));
	if (ret)
		return ret;

	*value = tmp;

	return 0;
}

static inline int everdrive_fifo_read_str(volatile void *fifo, unsigned char *str, size_t limit)
{
	uint16_t sz;
	int ret;

	ret = everdrive_fifo_read_u16(fifo, &sz);
	if (ret)
		return ret;

	if (limit < sz)
		return -ENOMEM;

	ret = everdrive_fifo_read(fifo, (void *) str, sz);
	if (ret)
		return ret;

	return sz;
}

static inline u16 everdrive_fifo_read_status(volatile void *fifo)
{
	u16 status;

        everdrive_fifo_write(fifo, (u8*) &cmd_status, sizeof(cmd_status));
        everdrive_fifo_read_u16(fifo, &status);

	return status;
}

static inline void everdrive_fifo_sendcmd(volatile void *fifo, const struct everdrive_fifo_cmd *cmd)
{
	/* If you don't break the fifo somehow this shouldn't be needed... */
	everdrive_drain(fifo);

	everdrive_fifo_write(fifo, (u8*) cmd, sizeof(*cmd));
}

#define EVERDRIVE_FIFO_MSG_FLAG_RETURN_U64 BIT(0)
#define EVERDRIVE_FIFO_MSG_FLAG_FOPEN BIT(1)
#define EVERDRIVE_FIFO_MSG_FLAG_ARG_U32 BIT(2)
#define EVERDRIVE_FIFO_MSG_FLAG_FRD BIT(3)
#define EVERDRIVE_FIFO_MSG_FLAG_USBWR BIT(4)
#define EVERDRIVE_FIFO_MSG_FLAG_TTYGET BIT(5)

struct everdrive_fifo_msg {
	/* Input values */
	const struct everdrive_fifo_cmd *cmd;
	unsigned int flags;

	union {
		u8 arg_u8;
		u16 arg_u16;
		u32 arg_u32;
	};

	union {
		const char *str;
		const void *arg_data;
	};

	/* Output values */
	int *ret;
	struct completion *done;
	u16 *status;

	union {
		u8 *return_u8;
		u64 *return_u64;
	};


	/* callbacks */
	void (*cb_str)(u8 *chars, size_t size);
	void (*cb_done)(void);

	void *return_data;
};

int everdrive_fifo_enqueue(const struct everdrive_fifo_msg *msg);

static inline int everdrive_fifo_enqueue_wait(struct everdrive_fifo_msg *msg)
{
	DECLARE_COMPLETION_ONSTACK(done);
	int ret, retret;

	msg->done = &done;
	msg->ret = &retret;

	ret = everdrive_fifo_enqueue(msg);
	if (ret)
		return ret;

	wait_for_completion(&done);

	return retret;
}

static inline bool everdrive_fifo_status_ok(u16 status)
{
	if ((status & 0xff00) == 0xa500)
		return true;

	return false;
}

#endif
