/*
 * Copyright (c) 2024 Sebastiano Barezzi <seba@sebaubuntu.dev>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/types.h>

#ifdef CONFIG_XIAOMI_KEYBOARD
void xiaomi_keyboard_connection_change(bool connected);
#else
static inline void xiaomi_keyboard_connection_change(bool connected) { }
#endif
