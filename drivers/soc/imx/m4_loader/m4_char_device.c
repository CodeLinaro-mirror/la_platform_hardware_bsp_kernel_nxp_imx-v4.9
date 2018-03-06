#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "m4_loader.h"

#define DEVICE_NAME "imx_m4_loader"
#define COPROC_NAME "M4"

static int driver_major;  /* Major number assigned to our device driver. */

static const int kImageMinor = 1;
static bool image_open;  /* if /image device is in use. */
static DEFINE_SPINLOCK(image_lock);
static int image_written; /* how much has been written in a session. */

static const int kBootMinor = 2;
static bool boot_open;  /* if /boot device is in use. */
static DEFINE_SPINLOCK(boot_lock);
static bool is_booted;  /* if the cortex is currently running. */

/*
 * Functions related to wrapping m4 loading code as a char device driver.
 */
static int open_device(struct inode *inode, struct file *file)
{
	const int minor_number = MINOR(inode->i_rdev);

	if (minor_number == kImageMinor) {
		spin_lock(&image_lock);
		if (image_open) {
			spin_unlock(&image_lock);
			return -EBUSY;
		}
		image_open = true;

		if (file->f_mode & FMODE_WRITE) {
			image_written = 0;

			/* Reset the platform to prepare to write. */
			imx_m4_reset_platform();
		}
		spin_unlock(&image_lock);
	} else if (minor_number == kBootMinor) {
		spin_lock(&boot_lock);
		if (boot_open) {
			spin_unlock(&boot_lock);
			return -EBUSY;
		}
		boot_open = true;
		spin_unlock(&boot_lock);
	} else {
		pr_warn("Unsupported minor number %d\n", minor_number);
		return -ENOENT;
	}

	return 0;
}

static int close_device(struct inode *inode, struct file *file)
{
	const int minor_number = MINOR(inode->i_rdev);

	if (minor_number == kImageMinor) {
		spin_lock(&image_lock);
		image_open = false;
		spin_unlock(&image_lock);
	} else if (minor_number == kBootMinor) {
		spin_lock(&boot_lock);
		boot_open = false;
		spin_unlock(&boot_lock);
	} else {
		pr_warn("Unsupported minor number %d\n", minor_number);
		return -ENOENT;
	}

	return 0;
}

static ssize_t read_device(struct file *file, char *buffer, size_t length,
			   loff_t *file_position)
{
	const int minor_number = MINOR(file->f_inode->i_rdev);

	if (minor_number == kImageMinor) {
		size_t copied = 0;
		const size_t adjusted_length =
			(length < (image_written - *file_position)) ?
				length : image_written - *file_position;
		char *kernel_buffer = kmalloc(adjusted_length, GFP_KERNEL);

		spin_lock(&image_lock);
		imx_m4_read_image(kernel_buffer, adjusted_length,
				  *file_position);
		spin_unlock(&image_lock);

		copied = copy_to_user(buffer, kernel_buffer, adjusted_length);
                kfree(kernel_buffer);

		*file_position += copied;
		return copied;
	}

	if (length == 0 || *file_position != 0 ||
	    !access_ok(VERIFY_READ, buffer, length)) {
		return 0;
	}

	if (minor_number == kBootMinor) {
		/*
		 * TODO maybe we should detect if the core has died on its
		 * own here too.
		 */
		if (put_user(is_booted ? '1' : '0', buffer) != 0) {
			pr_alert("Failed to write data from kernelspace "
				 "to userspace.\n");
			return 0;
		}
		*file_position += 1;
		return 1;
	}

	pr_warn("Unsupported minor number %d\n", minor_number);
	return 0;
}

static ssize_t write_device(struct file *file, const char *buffer,
			    size_t length, loff_t *file_position)
{
	const int minor_number = MINOR(file->f_inode->i_rdev);
	char *kernel_buf;
	bool boot_requested = false;
	char value = '\0';

	if (length == 0 || !access_ok(VERIFY_WRITE, buffer, length)) {
		return 0;
	}

	if (minor_number == kBootMinor) {
		if (get_user(value, buffer) != 0) {
			pr_alert("Failed to read data from userspace to "
				 "kernel space.\n");
			return 0;
		}
		boot_requested = (value == '1');

		spin_lock(&boot_lock);
		if ((boot_requested && is_booted)
		    || (!boot_requested && !is_booted)) {
			spin_unlock(&boot_lock);
			return length;
		}

		if (boot_requested) {
			imx_m4_boot();
			is_booted = true;
		} else {
			imx_m4_reset_platform();
			is_booted = false;
		}
		spin_unlock(&boot_lock);

		*file_position += 1;
		return length;
	}

	if (minor_number == kImageMinor) {
		kernel_buf = kmalloc(length, GFP_KERNEL);
		if (copy_from_user(kernel_buf, buffer, length) != 0) {
			pr_alert("Failed to read data from userspace to "
				 "kernel space.\n");
			kfree(kernel_buf);
			return 0;
		}

		/*
		 * Also acquire boot because we will shutdown processor when
		 * writing.
		 */
		spin_lock(&boot_lock);
		spin_lock(&image_lock);
		if (!imx_m4_write_image(kernel_buf, length, image_written)) {
			kfree(kernel_buf);
			spin_unlock(&image_lock);
			spin_unlock(&boot_lock);
			return 0;
		}
		kfree(kernel_buf);
		image_written += length;
		*file_position += length;
		is_booted = false;
		spin_unlock(&image_lock);
		spin_unlock(&boot_lock);
		return length;
	}

	pr_warn("Unsupported minor number %d\n", minor_number);
	return 0;
}

static const struct file_operations operations = {
	.open = open_device,
	.release = close_device,
	.read = read_device,
	.write = write_device
};


/**
 * Functions to load into udev and create /dev/ devices.
 */

static const struct of_device_id imx7d_m4_loader_match[] = {
	{ .compatible = "fsl,m4_data", },
	{}
};
MODULE_DEVICE_TABLE(of, imx7d_m4_loader_match);

static int probe(struct platform_device *platform)
{
	const struct of_device_id *match;
	struct class *cl = NULL;

	match = of_match_device(imx7d_m4_loader_match, &platform->dev);
	if (!match)
		return -EINVAL;

	driver_major = register_chrdev(0,  /* Request a major number. */
				 DEVICE_NAME,
				 &operations);
	if (driver_major < 0) {
		pr_alert("Registering m4 loader failed with %d\n",
			 driver_major);
		return driver_major;
	}

	cl = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(cl)) {
		pr_alert("Registering class failed.\n");
		class_destroy(cl);
		return -ENODEV;
	}

	if (device_create(cl, NULL, MKDEV(driver_major, kImageMinor), NULL,
			  "coproc"COPROC_NAME"Image") == 0) {
		pr_alert("Registering Image device failed.\n");
		class_destroy(cl);
		return -ENODEV;
	}

	if (device_create(cl, NULL, MKDEV(driver_major, kBootMinor), NULL,
			  "coproc"COPROC_NAME"Boot") == 0) {
		pr_alert("Registering Boot device failed.\n");
		class_destroy(cl);
		return -ENODEV;
	}

	imx_m4_init(platform);

	pr_info("M4 loader initialized with Major %d "
		"/dev/coproc"COPROC_NAME"*.\n",
		driver_major);
	return 0;
}

static struct platform_driver imx7d_m4_loader_driver = {
	.probe = probe,
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = imx7d_m4_loader_match,
	},
};

module_platform_driver(imx7d_m4_loader_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ed Coyne, Google Inc");
MODULE_DESCRIPTION("Module that provides an interface for loading a program "
		   "onto the cortex M4 core on an imx7d.");

