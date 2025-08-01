/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2011 Fujitsu.  All rights reserved.
 * Written by Miao Xie <miaox@cn.fujitsu.com>
 */

#ifndef BTRFS_DELAYED_INODE_H
#define BTRFS_DELAYED_INODE_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/refcount.h>
#include "ctree.h"

struct btrfs_disk_key;
struct btrfs_fs_info;
struct btrfs_inode;
struct btrfs_root;
struct btrfs_trans_handle;

enum btrfs_delayed_item_type {
	BTRFS_DELAYED_INSERTION_ITEM,
	BTRFS_DELAYED_DELETION_ITEM
};

struct btrfs_delayed_root {
	spinlock_t lock;
	struct list_head node_list;
	/*
	 * Used for delayed nodes which is waiting to be dealt with by the
	 * worker. If the delayed node is inserted into the work queue, we
	 * drop it from this list.
	 */
	struct list_head prepare_list;
	atomic_t items;		/* for delayed items */
	atomic_t items_seq;	/* for delayed items */
	int nodes;		/* for delayed nodes */
	wait_queue_head_t wait;
};

#define BTRFS_DELAYED_NODE_IN_LIST	0
#define BTRFS_DELAYED_NODE_INODE_DIRTY	1
#define BTRFS_DELAYED_NODE_DEL_IREF	2

struct btrfs_delayed_node {
	u64 inode_id;
	u64 bytes_reserved;
	struct btrfs_root *root;
	/* Used to add the node into the delayed root's node list. */
	struct list_head n_list;
	/*
	 * Used to add the node into the prepare list, the nodes in this list
	 * is waiting to be dealt with by the async worker.
	 */
	struct list_head p_list;
	struct rb_root_cached ins_root;
	struct rb_root_cached del_root;
	struct mutex mutex;
	struct btrfs_inode_item inode_item;
	refcount_t refs;
	int count;
	u64 index_cnt;
	unsigned long flags;
	/*
	 * The size of the next batch of dir index items to insert (if this
	 * node is from a directory inode). Protected by @mutex.
	 */
	u32 curr_index_batch_size;
	/*
	 * Number of leaves reserved for inserting dir index items (if this
	 * node belongs to a directory inode). This may be larger then the
	 * actual number of leaves we end up using. Protected by @mutex.
	 */
	u32 index_item_leaves;
};

struct btrfs_delayed_item {
	struct rb_node rb_node;
	/* Offset value of the corresponding dir index key. */
	u64 index;
	struct list_head tree_list;	/* used for batch insert/delete items */
	struct list_head readdir_list;	/* used for readdir items */
	/*
	 * Used when logging a directory.
	 * Insertions and deletions to this list are protected by the parent
	 * delayed node's mutex.
	 */
	struct list_head log_list;
	u64 bytes_reserved;
	struct btrfs_delayed_node *delayed_node;
	refcount_t refs;
	enum btrfs_delayed_item_type type:8;
	/*
	 * Track if this delayed item was already logged.
	 * Protected by the mutex of the parent delayed inode.
	 */
	bool logged;
	/* The maximum leaf size is 64K, so u16 is more than enough. */
	u16 data_len;
	char data[] __counted_by(data_len);
};

void btrfs_init_delayed_root(struct btrfs_delayed_root *delayed_root);
int btrfs_insert_delayed_dir_index(struct btrfs_trans_handle *trans,
				   const char *name, int name_len,
				   struct btrfs_inode *dir,
				   const struct btrfs_disk_key *disk_key, u8 flags,
				   u64 index);

int btrfs_delete_delayed_dir_index(struct btrfs_trans_handle *trans,
				   struct btrfs_inode *dir, u64 index);

int btrfs_inode_delayed_dir_index_count(struct btrfs_inode *inode);

int btrfs_run_delayed_items(struct btrfs_trans_handle *trans);
int btrfs_run_delayed_items_nr(struct btrfs_trans_handle *trans, int nr);

void btrfs_balance_delayed_items(struct btrfs_fs_info *fs_info);

int btrfs_commit_inode_delayed_items(struct btrfs_trans_handle *trans,
				     struct btrfs_inode *inode);
/* Used for evicting the inode. */
void btrfs_remove_delayed_node(struct btrfs_inode *inode);
void btrfs_kill_delayed_inode_items(struct btrfs_inode *inode);
int btrfs_commit_inode_delayed_inode(struct btrfs_inode *inode);


int btrfs_delayed_update_inode(struct btrfs_trans_handle *trans,
			       struct btrfs_inode *inode);
int btrfs_fill_inode(struct btrfs_inode *inode, u32 *rdev);
int btrfs_delayed_delete_inode_ref(struct btrfs_inode *inode);

/* Used for drop dead root */
void btrfs_kill_all_delayed_nodes(struct btrfs_root *root);

/* Used for clean the transaction */
void btrfs_destroy_delayed_inodes(struct btrfs_fs_info *fs_info);

/* Used for readdir() */
bool btrfs_readdir_get_delayed_items(struct btrfs_inode *inode,
				     u64 last_index,
				     struct list_head *ins_list,
				     struct list_head *del_list);
void btrfs_readdir_put_delayed_items(struct btrfs_inode *inode,
				     struct list_head *ins_list,
				     struct list_head *del_list);
bool btrfs_should_delete_dir_index(const struct list_head *del_list, u64 index);
bool btrfs_readdir_delayed_dir_index(struct dir_context *ctx,
				     const struct list_head *ins_list);

/* Used during directory logging. */
void btrfs_log_get_delayed_items(struct btrfs_inode *inode,
				 struct list_head *ins_list,
				 struct list_head *del_list);
void btrfs_log_put_delayed_items(struct btrfs_inode *inode,
				 struct list_head *ins_list,
				 struct list_head *del_list);

/* for init */
int __init btrfs_delayed_inode_init(void);
void __cold btrfs_delayed_inode_exit(void);

/* for debugging */
void btrfs_assert_delayed_root_empty(struct btrfs_fs_info *fs_info);

#endif
