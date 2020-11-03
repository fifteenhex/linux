// SPDX-License-Identifier: GPL-2.0
//

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <crypto/engine.h>
#include <crypto/sha2.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>

#include <crypto/internal/hash.h>

#define DRIVER_NAME "msc313-sha"
#define REG_CTRL	0x0
#define REG_SRC		0x8
#define REG_LEN		0x10
#define REG_MIUSEL	0x18
#define REG_STATUS	0x1c
#define REG_VALUE	0x20

struct msc313_sha_drv {
	struct list_head dev_list;
	/* Device list lock */
	spinlock_t lock;
};

static struct msc313_sha_drv msc313_sha = {
	.dev_list = LIST_HEAD_INIT(msc313_sha.dev_list),
	.lock = __SPIN_LOCK_UNLOCKED(msc313_sha.lock),
};

static const struct regmap_config msc313_sha_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4
};

static struct reg_field ctrl_fire = REG_FIELD(REG_CTRL, 0, 0);
static struct reg_field ctrl_clr = REG_FIELD(REG_CTRL, 6, 6);
static struct reg_field ctrl_rst = REG_FIELD(REG_CTRL, 7, 7);
static struct reg_field ctrl_sha256 = REG_FIELD(REG_CTRL, 9, 9);
static struct reg_field ctrl_disablesg = REG_FIELD(REG_CTRL, 11, 11);
static struct reg_field ctrl_inithash = REG_FIELD(REG_CTRL, 13, 13);
static struct reg_field status_ready = REG_FIELD(REG_STATUS, 0, 0);

struct msc313_sha {
	struct device *dev;
	struct list_head sha_list;
	struct crypto_engine *engine;
	struct clk *clk;
	struct regmap *regmap;

	struct regmap_field *fire;
	struct regmap_field *clear;
	struct regmap_field *reset;
	struct regmap_field *sha256;
	struct regmap_field *disablesg;
	struct regmap_field *inithash;
	struct regmap_field *ready;
};

struct msc313_sha_sha256_ctx
{
	struct msc313_sha *sha;
};

static struct msc313_sha *msc311_sha_find_dev(struct msc313_sha_sha256_ctx *tctx)
{
	struct msc313_sha *sha = NULL;
	struct msc313_sha *tmp;

	spin_lock_bh(&msc313_sha.lock);
	if (!tctx->sha) {
		list_for_each_entry(tmp, &msc313_sha.dev_list, sha_list) {
			sha = tmp;
			break;
		}
		tctx->sha = sha;
	} else {
		sha = tctx->sha;
	}

	spin_unlock_bh(&msc313_sha.lock);

	return sha;
}

static int msc313_sha_sha256_init(struct ahash_request *req)
{
	return 0;
}

static int msc313_sha_sha256_update(struct ahash_request *req)
{
	return 0;
}

static int msc313_sha_sha256_final(struct ahash_request *req)
{
	return 0;
}

static int msc313_sha_finup(struct ahash_request *req)
{
	return 0;
}

static int msc313_sha_digest(struct ahash_request *req)
{
	return 0;
}

static int msc313_sha_sha256_export(struct ahash_request *req, void *out)
{
	return 0;
}

static int msc313_sha_sha256_import(struct ahash_request *req, const void *in)
{
	return 0;
}

static struct ahash_alg msc313_algos[] = {
	{
		.init       = msc313_sha_sha256_init,
		.update     = msc313_sha_sha256_update,
		.final      = msc313_sha_sha256_final,
		.finup      = msc313_sha_finup,
		.digest	    = msc313_sha_digest,
		.export     = msc313_sha_sha256_export,
		.import     = msc313_sha_sha256_import,
		.halg	    = {
			.digestsize = SHA256_DIGEST_SIZE,
			.statesize  = sizeof(struct sha256_state),
			.base	    = {
					.cra_name        = "sha256",
					.cra_driver_name = "msc313-sha-sha256",
					.cra_priority    = 00,
					.cra_flags       = 0,
					.cra_blocksize   = SHA256_BLOCK_SIZE,
					.cra_module      = THIS_MODULE,
					.cra_ctxsize     = sizeof(struct msc313_sha_sha256_ctx),
			},
		},
	},
};

static u8 testdata[128] = {0};

static void msc313_sha_test(struct msc313_sha *sha)
{
	u32 len = sizeof(testdata);
	unsigned ready, res[16];
	int i;
	dma_addr_t dmaaddr = 0;

	dmaaddr = dma_map_single(sha->dev, testdata, len, DMA_TO_DEVICE);
	if (dma_mapping_error(sha->dev, dmaaddr))
		return;

	regmap_field_write(sha->reset, 1);
	clk_prepare_enable(sha->clk);
	mdelay(10);
	regmap_field_write(sha->reset, 0);

	regmap_field_write(sha->sha256, 1);
	//regmap_field_write(sha->inithash, 1);
	regmap_field_write(sha->disablesg, 1);

	regmap_write(sha->regmap, REG_SRC, dmaaddr);
	regmap_write(sha->regmap, REG_SRC + 4, dmaaddr >> 16);

	regmap_write(sha->regmap, REG_LEN, len);
	regmap_write(sha->regmap, REG_LEN + 4, len >> 16);

	regmap_field_write(sha->fire, 1);

	if(regmap_field_read_poll_timeout(sha->ready, ready, ready == 1, 2, 100))
		printk("timeout waiting for sha result\n");
	else {
		for(i = 0; i < ARRAY_SIZE(res); i++){
			regmap_read(sha->regmap, REG_VALUE + (i * 4), &res[i]);
			printk("value %d:%04x\n", i, res[i]);
		}
	}

	regmap_field_write(sha->fire, 0);
	regmap_field_write(sha->clear, 1);
	//clk_disable_unprepare(sha->clk);

	dma_unmap_single(sha->dev, dmaaddr, len, DMA_TO_DEVICE);
}

static int msc313_sha_probe(struct platform_device *pdev)
{
	struct msc313_sha *sha;
	void __iomem *base;
	int ret;

	sha = devm_kzalloc(&pdev->dev, sizeof(*sha), GFP_KERNEL);
	if (!sha){
		ret = -ENOMEM;
		goto out;
	}

	sha->dev = &pdev->dev;
	sha->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(sha->clk))
		return PTR_ERR(sha->clk);

	platform_set_drvdata(pdev, sha);

	INIT_LIST_HEAD(&sha->sha_list);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	sha->regmap = devm_regmap_init_mmio(&pdev->dev, base,
                        &msc313_sha_regmap_config);
	if(IS_ERR(sha->regmap))
		return PTR_ERR(sha->regmap);

	sha->fire = devm_regmap_field_alloc(&pdev->dev, sha->regmap, ctrl_fire);
	sha->clear = devm_regmap_field_alloc(&pdev->dev, sha->regmap, ctrl_clr);
	sha->reset = devm_regmap_field_alloc(&pdev->dev, sha->regmap, ctrl_rst);
	sha->sha256 = devm_regmap_field_alloc(&pdev->dev, sha->regmap, ctrl_sha256);
	sha->disablesg = devm_regmap_field_alloc(&pdev->dev, sha->regmap, ctrl_disablesg);
	sha->inithash = devm_regmap_field_alloc(&pdev->dev, sha->regmap, ctrl_inithash);
	sha->ready = devm_regmap_field_alloc(&pdev->dev, sha->regmap, status_ready);

	sha->engine = crypto_engine_alloc_init(&pdev->dev, 1);
	if (!sha->engine) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&msc313_sha.lock);
	list_add_tail(&sha->sha_list, &msc313_sha.dev_list);
	spin_unlock(&msc313_sha.lock);

	ret = crypto_engine_start(sha->engine);
	if (ret)
		goto out;

	ret = crypto_register_ahashes(msc313_algos, ARRAY_SIZE(msc313_algos));

	msc313_sha_test(sha);

out:
	return ret;
}

static int msc313_sha_remove(struct platform_device *pdev)
{
	struct msc313_sha *sha = platform_get_drvdata(pdev);

	crypto_unregister_ahashes(msc313_algos, ARRAY_SIZE(msc313_algos));
	crypto_engine_stop(sha->engine);

	return 0;
}

static const struct of_device_id msc313_sha_of_match[] = {
	{ .compatible = "mstar,msc313-sha", },
	{},
};
MODULE_DEVICE_TABLE(of, msc313_sha_of_match);

static struct platform_driver msc313_sha_driver = {
	.probe = msc313_sha_probe,
	.remove = msc313_sha_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_sha_of_match,
	},
};

module_platform_driver(msc313_sha_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 SHA driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
