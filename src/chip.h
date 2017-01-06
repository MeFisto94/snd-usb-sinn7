/*
 * Linux driver for Sinn7 Status 24|96 compatible devices
 *
 * Copyright 2016-2017 (C) Marc Streckfuß
 *
 * Authors:
 *           Marc Streckfuß <marc.streckfuss@gmail.com>
 *
 * The driver is based on the work done in the M2Tech hiFace Driver which
 * in turn is based on TerraTec DMX 6Fire USB.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SINN7_CHIP_H
#define SINN7_CHIP_H

#include <linux/usb.h>
#include <sound/core.h>

struct pcm_runtime;

struct sinn7_chip {
	struct usb_device *dev;
	struct snd_card *card;
	struct pcm_runtime *pcm;
};
#endif /* SINN7_CHIP_H */
