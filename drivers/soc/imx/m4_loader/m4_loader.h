#ifndef _M4_LOADER_H_
#define _M4_LOADER_H_

#include <linux/platform_device.h>

/*
 * Defines functions for loading m4 coprocessor on imx boards.
 */

/* Initialize module, load data from device tree. */
int imx_m4_init(struct platform_device *platform);

/* Boot coprocessor. */
void imx_m4_boot(void);

/* Reset coprocessor. */
void imx_m4_reset_platform(void);

/* Read coprocessor's image memory. */
bool imx_m4_read_image(char *buffer, const int length, const int image_offset);

/* Write to coprocessor's image memory. */
bool imx_m4_write_image(const char *buffer, const int length,
			const int image_offset);


#endif  /* _M4_LOADER_H_ */
