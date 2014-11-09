/* Copyright (c) 2010, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef MP3_FUNCS_H
#define MP3_FUNCS_H

/* Function Prototypes */
long mp3_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
void audpp_cmd_cfg_mp3_params(struct audio *audio);

#endif /* !MP3_FUNCS_H */
