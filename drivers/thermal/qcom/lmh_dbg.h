/* Copyright (c) 2014-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __QTI_LMH_H__
#define __QTI_LMH_H__

#include <linux/platform_device.h>

#ifdef CONFIG_DEBUG_KERNEL
int lmh_debug_register(struct platform_device *pdev);
#else
static inline int lmh_debug_register(struct platform_device *pdev)
{
	return 0;
}
#endif

#endif /* __QTI_LMH_H__ */
