// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/kfifo.h>
#include <linux/err.h>
#include <linux/printk.h>

#include <asm/everdrive.h>
#include <asm/everdrive-fifo.h>

static volatile void *everdrive_fifo_reg = (void *) MEGADRIVE_EVERDRIVE_MAILBOX;

#define EVERDRIVE_FIFO_DEPTH	256

/* mutex for the hw access */
DEFINE_MUTEX(everdrive_fifo_mutex);
/* for the fifo */
static DEFINE_SPINLOCK(everdrive_fifo_lock);

static DECLARE_KFIFO(everdrive_fifo, struct everdrive_fifo_msg,
		     EVERDRIVE_FIFO_DEPTH);
static DECLARE_WAIT_QUEUE_HEAD(everdrive_fifo_waitq);
static struct task_struct *everdrive_fifo_thread;

int everdrive_fifo_enqueue(const struct everdrive_fifo_msg *msg)
{
	int ret = 0;

	if (!kfifo_in_spinlocked(&everdrive_fifo, msg, 1, &everdrive_fifo_lock))
		ret = -ENOSPC;

	if (!ret)
		wake_up_interruptible(&everdrive_fifo_waitq);

	return ret;
}

static int everdrive_fifo_process_msg(struct everdrive_fifo_msg *msg)
{
	int ret = 0;

	scoped_cond_guard(mutex_try, return -EAGAIN, &everdrive_fifo_mutex) {

		if (msg->flags & EVERDRIVE_FIFO_MSG_FLAG_TTYGET) {
			// basically drain for now ..
			unsigned int level;
			u8 tmp[64];

			while ((level = everdrive_fifo_level(everdrive_fifo_reg))) {
				unsigned int readsz = min(sizeof(tmp), level);

				/* We already know how much we want, use the naked read */
				_everdrive_fifo_read(everdrive_fifo_reg, tmp, readsz);
				msg->cb_str(tmp, readsz);
			}
			msg->cb_done();
                }
		else
			everdrive_fifo_sendcmd(everdrive_fifo_reg, msg->cmd);

		if (msg->flags & EVERDRIVE_FIFO_MSG_FLAG_RETURN_U64)
			ret = everdrive_fifo_read_u64(everdrive_fifo_reg, msg->return_u64);
		else if (msg->flags & EVERDRIVE_FIFO_MSG_FLAG_ARG_U32)
			everdrive_fifo_write_u32(everdrive_fifo_reg, msg->arg_u32);
		else if (msg->flags & EVERDRIVE_FIFO_MSG_FLAG_FOPEN) {
			everdrive_fifo_write_u8(everdrive_fifo_reg, msg->arg_u8);
			everdrive_fifo_write_str(everdrive_fifo_reg, msg->str, strlen(msg->str));
		}
		else if(msg->flags & EVERDRIVE_FIFO_MSG_FLAG_FRD) {
                	everdrive_fifo_write_u32(everdrive_fifo_reg, msg->arg_u32);

			ret = everdrive_fifo_read_u8(everdrive_fifo_reg, msg->return_u8);
			if (ret)
				goto out;

			/* I think is resp is not 0 we should actually abort here !! */
			ret = everdrive_fifo_read(everdrive_fifo_reg, msg->return_data, msg->arg_u32);
			if (ret)
				goto out;
		}
		else if(msg->flags & EVERDRIVE_FIFO_MSG_FLAG_USBWR) {
                	everdrive_fifo_write_u16(everdrive_fifo_reg, msg->arg_u16);
			everdrive_fifo_write(everdrive_fifo_reg, msg->arg_data, msg->arg_u16);
		}

		if (msg->status)
			*msg->status = everdrive_fifo_read_status(everdrive_fifo_reg);
	}

out:
	if (ret)
		printk("ut oh %d!\n", ret);

	if (msg->ret)
		*msg->ret = ret;

	return 0;
}

static int everdrive_fifo_service(void *unused)
{
	struct everdrive_fifo_msg msg;
	int ret;

	while (!kthread_should_stop()) {
		wait_event_interruptible(everdrive_fifo_waitq,
					 !kfifo_is_empty(&everdrive_fifo) ||
					 kthread_should_stop());

		if (kthread_should_stop())
			break;

		while (kfifo_out(&everdrive_fifo, &msg, 1) == 1) {
			while (true) {
				ret = everdrive_fifo_process_msg (&msg);
				if (ret == -EAGAIN)
					continue;

				*msg.ret = ret;

				break;
			}
			if (msg.done)
				complete(msg.done);
		}
	}

	return 0;
}

static int __init everdrive_fifo_init(void)
{
	INIT_KFIFO(everdrive_fifo);

	everdrive_fifo_thread = kthread_run(everdrive_fifo_service, NULL,
					    "everdrive_fifo");
	if (IS_ERR(everdrive_fifo_thread))
		return PTR_ERR(everdrive_fifo_thread);

	printk("everdrive fifo thread started\n");

	return 0;
}

early_initcall(everdrive_fifo_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EverDrive fifo driver");
