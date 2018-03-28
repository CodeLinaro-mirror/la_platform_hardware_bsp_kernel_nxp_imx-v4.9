#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uio_driver.h>

#define DEVICE_NAME "memrt"
#define DEVICE_VERSION "1.0"

static DEFINE_SPINLOCK(is_open_lock);
static bool is_open;  // Initialized to false by kernel.

static const struct of_device_id memrt_match[] = {
	{ .compatible = "android,memrt", },
	{}
};

static int open(struct uio_info *uio, struct inode *node)
{
	spin_lock(&is_open_lock);
	if (is_open) {
		spin_unlock(&is_open_lock);
		return -EBUSY;
	}
	is_open = true;
	spin_unlock(&is_open_lock);
	return 0;
}

static int close(struct uio_info *uio, struct inode *node)
{
	spin_lock(&is_open_lock);
	is_open = false;
	spin_unlock(&is_open_lock);
	return 0;
}

static int probe(struct platform_device *platform)
{
	const struct of_device_id *match = NULL;
	struct uio_info *uio = NULL;
	u64 size = 0;

	match = of_match_device(memrt_match, &platform->dev);
	if (!match)
		return -EINVAL;

	uio = devm_kzalloc(&platform->dev, sizeof(struct uio_info), GFP_KERNEL);
	uio->name = DEVICE_NAME;
	uio->version = DEVICE_VERSION;
	uio->irq = UIO_IRQ_NONE;
	uio->open = open;
	uio->release = close;

	uio->mem[0].memtype = UIO_MEM_PHYS;
	uio->mem[0].addr =
		of_translate_address(platform->dev.of_node,
				     of_get_address(platform->dev.of_node, 0,
						    &size, NULL));
	uio->mem[0].size = (resource_size_t)size;

	if (!uio->mem[0].addr || uio->mem[0].addr == OF_BAD_ADDR) {
		pr_err("MemRt failed to map shared memory.\n");
		return -ENOMEM;
	}

	return uio_register_device(&platform->dev, uio);
}

MODULE_DEVICE_TABLE(of, memrt_match);

static struct platform_driver memrt_driver = {
	.probe = probe,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = memrt_match,
	},
};

module_platform_driver(memrt_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ed Coyne, Google Inc.");
MODULE_DESCRIPTION("Shared memory serial channel between processors.");
