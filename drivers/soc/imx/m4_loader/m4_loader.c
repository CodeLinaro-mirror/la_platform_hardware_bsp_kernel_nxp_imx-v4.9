#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#define SRC_M4RCR      0x3039000C
#define OCRAM_S        0x00180000

#define SRC_M4RCR_M4C_NON_SCLR_RST_OFFSET 0
#define SRC_M4RCR_M4C_NON_SCLR_RST_MASK (1 << 0)
#define SRC_M4RCR_ENABLE_M4_OFFSET 3
#define SRC_M4RCR_ENABLE_M4_MASK (1 << 3)
#define SRC_M4RCR_M4C_RST_MASK (1 << 1)

/* These will be loaded from device tree. */
static void __iomem *m4_data;
static uint32_t m4_data_size;

void imx_m4_boot(void)
{
	void __iomem *m4rcr = ioremap_nocache(SRC_M4RCR, 4);

	iowrite32((ioread32(m4rcr) & ~(SRC_M4RCR_M4C_NON_SCLR_RST_MASK)) |
		  SRC_M4RCR_ENABLE_M4_MASK, m4rcr);

	iowrite32((ioread32(m4rcr) | SRC_M4RCR_M4C_RST_MASK), m4rcr);

	pr_info("Booted cortex M4\n");
	iounmap(m4rcr);
}

void imx_m4_reset_platform(void)
{
	int millis_slept = 0;
	void __iomem *m4rcr = ioremap_nocache(SRC_M4RCR, 4);

	/*
	 * Issue a platform reset by setting the SRC_M4RCR[2] bit
	 * in SRC_MRCR register.
	 */
	iowrite32(ioread32(m4rcr) | (1 << 2), m4rcr);

	/* Wait for the SRC_M4RCR[2] to be cleared by other processor */
	while ((ioread32(m4rcr) & (1 << 2)) != 0) {
		msleep(10);
		millis_slept += 10;
		if (millis_slept > 2000) {
			pr_err("Timeout waiting for m4 to clear "
			       " SRC_M4RCR[2].\n");
			iounmap(m4rcr);
			return;
		}
	}
	iounmap(m4rcr);
	pr_info("Cortex M4 platform reset\n");
}

bool imx_m4_read_image(char *buffer, const int length, const int image_offset)
{
	if (image_offset + length >= m4_data_size) {
		pr_err("Image file is too long, max is %u.\n",
		       m4_data_size);
		return false;
	}

	memcpy_fromio(buffer, m4_data + image_offset, length);
	return true;
}


bool imx_m4_write_image(const char *buffer, const int length,
			const int image_offset)
{
	void __iomem *ocram_s = NULL;

	if (image_offset + length >= m4_data_size) {
		pr_err("Image file is too long, only support up to %u.\n",
		       m4_data_size);
		return false;
	}

	/* Load image into DDR. */
	memcpy_toio(m4_data + image_offset, buffer, length);

	ocram_s = ioremap(OCRAM_S, 8);
	/* Only set Stack ptr and PC on first chunk. */
	if (image_offset == 0) {
		/*
		 * Set the Stack pointer to the first four bytes of the binary
		 * file and Set the PC pointer to next four bytes after the
		 * first four bytes of the binary file.
		 */
		memcpy_toio(ocram_s, buffer, 8);
	}

	iounmap(ocram_s);

	return true;
}

int imx_m4_init(struct platform_device *platform)
{
	int result = 0;
	struct resource res;

	result = of_address_to_resource(platform->dev.of_node, 0, &res);
	if (result) {
		pr_err("M4 loader failed to get resource from devtree %d.\n",
		       result);
		return -ENODEV;
	}

	m4_data = of_iomap(platform->dev.of_node, 0);
	if (!m4_data) {
		pr_err("M4 loader failed to load memory from devtree.\n");
		return -ENOMEM;
	}
	m4_data_size = resource_size(&res);
	return 0;
}
