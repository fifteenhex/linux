#ifndef __EVERDRIVE_H
#define __EVERDRIVE_H

#include <asm/io.h>

#define MEGADRIVE_EVERDRIVE_MAILBOX 0xa130d0

static inline void everdrive_usb_write(u8 value)
{
	asm volatile (
		"move.w #0x2b, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0xd4, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0x22, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0xdd, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0x00, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0x01, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w %0,  " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
	: : "d" (value));
}

static inline void everdrive_usb_write_buf(const u8 *buf, u16 len)
{
	u16 i;

	asm volatile (
		"move.w #0x2b, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0xd4, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0x22, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w #0xdd, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		"move.w %0,    " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"  /* len high */
		"move.w %1,    " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"  /* len low  */
	: : "d" (len >> 8), "d" (len & 0xff));

	for (i = 0; i < len; i++)
		asm volatile (
			"move.w %0, " STR(MEGADRIVE_EVERDRIVE_MAILBOX) "\n"
		: : "d" (buf[i]));
}

#endif /* EVERDRIVE_H */
