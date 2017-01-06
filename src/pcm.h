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

#ifndef SINN7_PCM_H
#define SINN7_PCM_H

struct sinn7_chip;

int sinn7_pcm_init(struct sinn7_chip *chip, u8 extra_freq);
void sinn7_pcm_abort(struct sinn7_chip *chip);
#endif /* SINN7_PCM_H */
