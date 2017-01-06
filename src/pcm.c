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

#include <linux/slab.h>
#include <linux/timer.h>
#include <sound/pcm.h>
#include <linux/usb.h>

#include "pcm.h"
#include "chip.h"

#define OUT_EP          0x5
#define PCM_N_URBS      8
#define PCM_BLOCK_SIZE	512
#define MAX_PACKET_SIZE 19968
#define PCM_BUFFER_SIZE (2 * PCM_N_URBS * MAX_PACKET_SIZE)

struct pcm_urb {
	struct sinn7_chip *chip;

	struct urb instance;
	struct usb_anchor submitted;
	u8 *buffer;
};

struct pcm_substream {
	spinlock_t lock;
	struct snd_pcm_substream *instance;

	bool active;
	snd_pcm_uframes_t dma_off;    /* current position in alsa dma_area */
	snd_pcm_uframes_t period_off; /* current position in current period */
};

enum { /* pcm streaming states */
	STREAM_DISABLED, /* no pcm streaming */
	STREAM_STARTING, /* pcm streaming requested, waiting to become ready */
	STREAM_RUNNING,  /* pcm streaming running */
	STREAM_STOPPING
};

struct pcm_runtime {
	struct sinn7_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	bool panic; /* if set driver won't do anymore pcm on device */

	struct pcm_urb out_urbs[PCM_N_URBS];

	struct mutex stream_mutex;
	u8 stream_state; /* one of STREAM_XXX */
	u8 extra_freq;
	wait_queue_head_t stream_wait_queue;
	bool stream_wait_cond;
	
	struct timer_list *timer;
};

//static const unsigned int rates[] = { 44100, 48000, /* ?? 88200, */96000};
/*static const struct snd_pcm_hw_constraint_list constraints_extra_rates = {
	.count = ARRAY_SIZE(rates),
	.list = rates,
	.mask = 0,
};*/

static const struct snd_pcm_hardware pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		//SNDRV_PCM_INFO_BLOCK_TRANSFER |
		//SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_MMAP_VALID,
		//SNDRV_PCM_INFO_BATCH,

	.formats = SNDRV_PCM_FMTBIT_S16_LE,// | SNDRV_PCM_FMTBIT_S24_BE,

	.rates = SNDRV_PCM_RATE_44100,// |
		//SNDRV_PCM_RATE_48000 |
		/* ?? SNDRV_PCM_RATE_88200 | */
		//SNDRV_PCM_RATE_96000,

	.rate_min = 44100,
	.rate_max = 44100,
	//.rate_max = 96000,
	.channels_min = 2,
	.channels_max = 2,
	
	/* Note: The following parameters only relate to the buffer
	 * we get from userspace. It's okay that our device has slightly
	 * other periods and such
	 */
	
	/* TODO: Change this value based on the FMT? Currently we picked
	 * 24 bits to be able to send 250 Frames in 24bit mode. That
	 * however means that we will have more frames in 16bit mode
	 */
	
	.buffer_bytes_max = PCM_BUFFER_SIZE,
	
	// Frames * Byte pro Frame * # Channels
	.period_bytes_min = 250 * 2 * 2, // 250 Frames (12800 Byte BULK Data)
	.period_bytes_max = 390 * 2 * 2, // 390 Frames (19968 Byte BULK Data)
	.periods_min = 1,
	.periods_max = 200, // 166 periods equal 1 second
};

void sinn7_timer_interrupt(unsigned long data);

/* message values used to change the sample rate */
#define HIFACE_SET_RATE_REQUEST 0xb0

#define HIFACE_RATE_44100  0x43
#define HIFACE_RATE_48000  0x4b
#define HIFACE_RATE_88200  0x42
#define HIFACE_RATE_96000  0x4a
#define HIFACE_RATE_176400 0x40
#define HIFACE_RATE_192000 0x48
#define HIFACE_RATE_352800 0x58
#define HIFACE_RATE_384000 0x68

static inline size_t sinn7_framecount_to_buffersize(const uint32_t numFrames) {
	// See sinn7_frames_to_buffer for the calculation base
	return ((numFrames / 10) + (numFrames % 10 > 0 ? 1 : 0)) * PCM_BLOCK_SIZE;
}

/**
 * This method converts a default PCM frame into an usb-ready sinn7 frame.
 * 
 * @param resultBuffer The Buffer to store the result at, offset to the correct position
 * @param frameBuffer The Buffer to read two frames of (left, right), offset to the correct position
 * @param bytesPerFrame The Number of bytes each frame consists of (equals to Bitness * 8).
 */
static void sinn7_frame_to_buffer(void *resultBuffer, void *frameBuffer, uint8_t bytesPerFrame) {
	/* For loop counters */
	uint8_t i;
	uint8_t j;
	int8_t k;
	
	int32_t frame;
	
	for (i = 0; i < 2; i++) { /* Stereo */
		if (bytesPerFrame == 2) {
			const uint8_t frame_01 = *((uint8_t*)(frameBuffer + i * bytesPerFrame    ));
			const uint8_t frame_02 = *((uint8_t*)(frameBuffer + i * bytesPerFrame + 1));
			frame = le32_to_cpu(((frame_02 & 0xFF) << 8) | (frame_01 & 0xFF));
			//frame = *((int16_t*)(frameBuffer + i * bytesPerFrame));
			//printk("frame == %d\n", (int16_t)(frame & 0xFFFF));
		} else if (bytesPerFrame == 3) { /* Manual read since there is no uint24_t */
			/* We read it as little endian, since most desktops run that,
			 * so the conversion is only run on a minority of machines
			 */
			const uint8_t frame_01 = *((uint8_t*)(frameBuffer + i * bytesPerFrame    ));
			const uint8_t frame_02 = *((uint8_t*)(frameBuffer + i * bytesPerFrame + 1));
			const uint8_t frame_03 = *((uint8_t*)(frameBuffer + i * bytesPerFrame + 2));
			
			frame = le32_to_cpu(((frame_03 & 0xFF) << 16) | ((frame_02 & 0xFF) << 8) || (frame_01 & 0xFF));
		} else {
			printk("FATAL: Invalid bytesPerFrame=%d specified, Invalid Format.\n", bytesPerFrame);
			return;
		}
			
		
		// Now we need to write the Frame as BIG ENDIAN and Encode the bits as bytes, so:
		for (j = 0; j < 3; j++) { /* Per byte of frame. */
			uint8_t byte = 0x0;
			
			if (bytesPerFrame == 3) { /* 24 bits */
				if (j == 0) {
					/*if (frame < 0) {
						frame |= 0x800000;
					}*/
					byte = (frame & 0xFF0000) >> 16; /* HIGH-Byte */
				} else if (j == 1) {
					byte = (frame & 0x00FF00) >> 8; /* MIDDLE-Byte */
				} else {
					byte = (frame & 0x0000FF); /* LOW-BYTE */
				}
			} else if (bytesPerFrame == 2) { /* 16 bits */
				if (j == 0) {
					/* Since we used int32_t, we need to shift the sign */
					/*if (frame < 0) {
						frame |= 0x8000;
					}*/
					
					byte = (frame & 0xFF00) >> 8; /* HIGH-Byte */
				} else if (j == 1) {
					byte = (frame & 0x00FF); /* LOW-BYTE */
				} else {
					byte = 0x0;
				}
			}
			
			void *outBuffer = (resultBuffer + i * 3 * 8 + j * 8);
			
			for (k = 7; k >= 0; k--) {
				memset(outBuffer, (byte & (1 << k)) != 0 ? 0x1 : 0x0, 1);
				outBuffer++;
			}
        }
	}
}

/**
 * This method converts a default PCM sequence into an usb-ready buffer to be used with the SINN7 Interface.
 * 
 * @param frameBuffer The Address where the frames are stored. It's size has to equal numFrames * bytesPerFrame * 2
 * @param numFrames The Number of frames in the framebuffer.
 * @param bytesPerFrame The Number of bytes each frame consists of (Equal to Bitness * 8). Has to be 2 currently.
 * @return An usb-ready buffer to be sent to the interface.
 */
static void *sinn7_frames_to_buffer(void *frameBuffer, uint32_t numFrames, uint8_t bytesPerFrame)
{
	uint32_t blockId;
	//Note: xyzBytesPerFrame is on a per Channel Base, so we need to multiply it by 2, since we're in stereo mode.
	const uint8_t outputBytesPerFrame = 24; // Even in 16bit mode, we output 24bit (And yes, here 1 Bit == 1 Device Byte)
	const uint8_t paddingFrameSize = 32; // After 10 Frames, we have to pad with these bytes.
	const uint32_t blockSize = outputBytesPerFrame * 2 * 10 + paddingFrameSize;
	
	const uint32_t numOfBlocks = (numFrames / 10) + (numFrames % 10 > 0 ? 1 : 0); // Each "Block" contains 10 Frames and ends with a Padding Frame
	const size_t outputBufferSize = numOfBlocks * blockSize;

	void *buf = kzalloc(outputBufferSize, GFP_ATOMIC);
	
	for (blockId = 0; blockId < numOfBlocks; blockId++) {
		uint8_t frameId;
		uint8_t currentFrames;
		
		currentFrames = (numFrames - blockId * blockSize) > 10 ? 10 : (numFrames - blockId * blockSize);
		
		for (frameId = 0; frameId < currentFrames; frameId++) {
			sinn7_frame_to_buffer((void*)(buf + blockId * blockSize + frameId * outputBytesPerFrame * 2),
					      (frameBuffer + blockId * bytesPerFrame * 2 * 10 + frameId * bytesPerFrame * 2),
					      bytesPerFrame);
		}
		
		memset(buf + blockId * blockSize + 10 * outputBytesPerFrame * 2    , 0xFD, 1 );
		memset(buf + blockId * blockSize + 10 * outputBytesPerFrame * 2 + 1, 0xFF, 1 );
		memset(buf + blockId * blockSize + 10 * outputBytesPerFrame * 2 + 2, 0x00, 30);
	}
	
	return buf;
}

/**
 * This method converts a default PCM sequence into an usb-ready buffer to be used with the SINN7 Interface.
 *  
 * @param frameBuffer The Address where the frames are stored. It's size has to equal numFrames * bytesPerFrame * 2
 * @param numFrames The Number of frames in the framebuffer.
 * @param bytesPerFrame The Number of bytes each frame consists of (Equal to Bitness * 8). Has to be 2 currently.
 * @param targetBuffer The Buffer for the output data
 */
static void sinn7_frames_to_buffer_ex(void *frameBuffer, uint32_t numFrames, uint8_t bytesPerFrame, void *targetBuffer)
{
	void *buf;
	buf = sinn7_frames_to_buffer(frameBuffer, numFrames, bytesPerFrame);
	memcpy(targetBuffer, buf, sinn7_framecount_to_buffersize(numFrames));
	kfree(buf);
}

static int sinn7_chip_pcm_set_rate(struct pcm_runtime *rt, unsigned int rate)
{	
	return 0; /* TODO: Implement */
	
	struct usb_device *device = rt->chip->dev;
	u16 rate_value;
	int ret;

	/* We are already sure that the rate is supported here thanks to
	 * ALSA constraints
	 */
	switch (rate) {
	case 44100:
		rate_value = HIFACE_RATE_44100;
		break;
	case 48000:
		rate_value = HIFACE_RATE_48000;
		break;
	case 88200:
		rate_value = HIFACE_RATE_88200;
		break;
	case 96000:
		rate_value = HIFACE_RATE_96000;
		break;
	case 176400:
		rate_value = HIFACE_RATE_176400;
		break;
	case 192000:
		rate_value = HIFACE_RATE_192000;
		break;
	case 352800:
		rate_value = HIFACE_RATE_352800;
		break;
	case 384000:
		rate_value = HIFACE_RATE_384000;
		break;
	default:
		dev_err(&device->dev, "Unsupported rate %d\n", rate);
		return -EINVAL;
	}

	/*
	 * USBIO: Vendor 0xb0(wValue=0x0043, wIndex=0x0000)
	 * 43 b0 43 00 00 00 00 00
	 * USBIO: Vendor 0xb0(wValue=0x004b, wIndex=0x0000)
	 * 43 b0 4b 00 00 00 00 00
	 * This control message doesn't have any ack from the
	 * other side
	 */
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0),
			      HIFACE_SET_RATE_REQUEST,
			      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_OTHER,
			      rate_value, 0, NULL, 0, 100);
	if (ret < 0) {
		dev_err(&device->dev, "Error setting samplerate %d.\n", rate);
		return ret;
	}

	return 0;
}

static struct pcm_substream *sinn7_pcm_get_substream(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct device *device = &rt->chip->dev->dev;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &rt->playback;

	dev_err(device, "Error getting pcm substream slot.\n");
	return NULL;
}

/* call with stream_mutex locked */
static void sinn7_pcm_stream_stop(struct pcm_runtime *rt)
{
	int i, time;
	
	if (rt->timer != 0x0) {
		del_timer_sync(rt->timer);
		kfree(rt->timer);
		rt->timer = 0x0;
	}

	if (rt->stream_state != STREAM_DISABLED) {
		rt->stream_state = STREAM_STOPPING;

		for (i = 0; i < PCM_N_URBS; i++) {
			time = usb_wait_anchor_empty_timeout(&rt->out_urbs[i].submitted, 100);
			if (!time) {
				usb_kill_anchored_urbs(
					&rt->out_urbs[i].submitted);
			}
			//usb_unlink_urb(&rt->out_urbs[i]->instance);
			usb_kill_urb(&rt->out_urbs[i].instance);
		}

		rt->stream_state = STREAM_DISABLED;
	}
}

/* call with stream_mutex locked */
static int sinn7_pcm_stream_start(struct pcm_runtime *rt)
{
	int ret = 0;
	int i;

	if (rt->stream_state == STREAM_DISABLED) {

		/* reset panic state when starting a new stream */
		rt->panic = false;

		/* submit our out urbs zero init */
		rt->stream_state = STREAM_STARTING;
			
		const size_t bufSize = sinn7_framecount_to_buffersize(250);
		
		void *zeroFrames = kzalloc(250 * 2 * 2, GFP_ATOMIC);

		void *buffer = sinn7_frames_to_buffer(zeroFrames, 250, 2);
		kfree(zeroFrames);
		
		struct device *device = &rt->chip->dev->dev;
			dev_dbg(device, "%s: Stream is running wakeup event\n",
				 __func__);
		rt->stream_state = STREAM_RUNNING;
		return 0;
			
		for (i = 0; i < PCM_N_URBS; i++) {
			memcpy(rt->out_urbs[i].buffer, buffer, bufSize);
			rt->out_urbs[i].instance.transfer_buffer_length = bufSize;
			
			usb_anchor_urb(&rt->out_urbs[i].instance,
				       &rt->out_urbs[i].submitted);
			//printk("anchor usb\n");
			//return -EIO;
			ret = usb_submit_urb(&rt->out_urbs[i].instance,
					     GFP_ATOMIC); // -22

			if (ret != 0) {
				if (ret == -EINVAL) {
					dev_err(&rt->chip->dev->dev, "Unable to submit URB, maybe the Endpoint is invalid?\n");
				}
				
				sinn7_pcm_stream_stop(rt);
				kfree(buffer);
				return ret;
			}
		}
		
		kfree(buffer);		
		
		/* wait for first out urb to return (sent in in urb handler) */
		wait_event_timeout(rt->stream_wait_queue, rt->stream_wait_cond,
				   HZ);
		
		if (rt->stream_wait_cond) {
			struct device *device = &rt->chip->dev->dev;
			dev_dbg(device, "%s: Stream is running wakeup event\n",
				 __func__);

            rt->stream_state = STREAM_RUNNING;
		} else {
			sinn7_pcm_stream_stop(rt);
			return -EIO;
		}
	}
	
	return ret;
}


/* call with substream locked */
/* returns true if a period elapsed */
static bool sinn7_pcm_playback(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	struct device *device = &urb->chip->dev->dev;
	u8 *source;
	unsigned int pcm_buffer_size;
	size_t period_bytes;
	
	period_bytes = frames_to_bytes(alsa_rt, alsa_rt->period_size); /* The chunk we process */

	WARN_ON(alsa_rt->format != SNDRV_PCM_FORMAT_S16_LE);
	pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	if (sub->dma_off + period_bytes <= pcm_buffer_size) {
		dev_dbg(device, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__,
			 (unsigned int) pcm_buffer_size,
			 (unsigned int) sub->dma_off);

		source = alsa_rt->dma_area + sub->dma_off;
		memcpy(urb->buffer, source, period_bytes);
	} else {
		/* wrap around at end of ring buffer */
		unsigned int len;

		dev_dbg(device, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__,
			 (unsigned int) pcm_buffer_size,
			 (unsigned int) sub->dma_off);

		len = pcm_buffer_size - sub->dma_off;

		source = alsa_rt->dma_area + sub->dma_off;
		memcpy(urb->buffer, source, len);

		source = alsa_rt->dma_area;
		memcpy(urb->buffer + len, source, period_bytes - len);
	}
	
	sub->dma_off += period_bytes;
	if (sub->dma_off >= pcm_buffer_size)
		sub->dma_off -= pcm_buffer_size;

	sub->period_off += alsa_rt->period_size;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}
	return false;
}

static void sinn7_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct pcm_runtime *rt = out_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT ||	/* unlinked */
		     usb_urb->status == -ENODEV ||	/* device removed */
		     usb_urb->status == -ECONNRESET ||	/* unlinked */
		     usb_urb->status == -ESHUTDOWN)) {	/* device disabled */
		goto out_fail;
	}

	if (rt->stream_state == STREAM_STARTING) {
		rt->stream_wait_cond = true;
		wake_up(&rt->stream_wait_queue);
	}
}

static int sinn7_pcm_open(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = NULL;
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;

	if (rt->panic)
		return -EPIPE;

	mutex_lock(&rt->stream_mutex);
	alsa_rt->hw = pcm_hw;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		sub = &rt->playback;

	if (!sub) {
		struct device *device = &rt->chip->dev->dev;
		mutex_unlock(&rt->stream_mutex);
		dev_err(device, "Invalid stream type\n");
		return -EINVAL;
	}

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int sinn7_pcm_close(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = sinn7_pcm_get_substream(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return 0;

	mutex_lock(&rt->stream_mutex);
	if (sub) {
		sinn7_pcm_stream_stop(rt);

		/* deactivate substream */
		spin_lock_irqsave(&sub->lock, flags);
		sub->instance = NULL;
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);

	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int sinn7_pcm_hw_params(struct snd_pcm_substream *alsa_sub,
				struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_alloc_vmalloc_buffer(alsa_sub,
						params_buffer_bytes(hw_params));
}

static int sinn7_pcm_hw_free(struct snd_pcm_substream *alsa_sub)
{
	return snd_pcm_lib_free_vmalloc_buffer(alsa_sub);
}

static int sinn7_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = sinn7_pcm_get_substream(alsa_sub);
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	int ret;
	bool wasDisabled;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);

	sub->dma_off = 0;
	sub->period_off = 0;

	if (rt->stream_state == STREAM_DISABLED) {
		wasDisabled = true; // preserve, since the state might change
		
		ret = sinn7_chip_pcm_set_rate(rt, alsa_rt->rate);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
		ret = sinn7_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
	} else {
		wasDisabled = false;
	}
	
	mutex_unlock(&rt->stream_mutex);
	
	if (wasDisabled && rt->timer == 0x0) {
		rt->timer = (struct timer_list *)kzalloc(sizeof(struct timer_list), GFP_ATOMIC);
        
		setup_timer(rt->timer, sinn7_timer_interrupt, (unsigned long)rt);
		rt->timer->expires = jiffies + msecs_to_jiffies(7);
		add_timer(rt->timer);
	}
	
	return 0;
}

static int sinn7_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
	struct pcm_substream *sub = sinn7_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&sub->lock, flags);
		sub->active = true;
		spin_unlock_irqrestore(&sub->lock, flags);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&sub->lock, flags);
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t sinn7_pcm_pointer(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_substream *sub = sinn7_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t dma_offset;

	if (rt->panic || !sub)
		return SNDRV_PCM_POS_XRUN;

	spin_lock_irqsave(&sub->lock, flags);
	dma_offset = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);
	return bytes_to_frames(alsa_sub->runtime, dma_offset);
}

static struct snd_pcm_ops pcm_ops = {
	.open = sinn7_pcm_open,
	.close = sinn7_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = sinn7_pcm_hw_params,
	.hw_free = sinn7_pcm_hw_free,
	.prepare = sinn7_pcm_prepare,
	.trigger = sinn7_pcm_trigger,
	.pointer = sinn7_pcm_pointer,
	.page = snd_pcm_lib_get_vmalloc_page,
	.mmap = snd_pcm_lib_mmap_vmalloc,
};

static void sinn7_flush_buffers(struct urb *usb_urb, struct pcm_urb *out_urb, struct pcm_runtime *rt, unsigned long lock_flags)
{
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	int ret;
	
	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;
	
	/* now send our playback data (if a free out urb was found) */
	sub = &rt->playback;

	if (sub->active) {
		do_period_elapsed = sinn7_pcm_playback(sub, out_urb);
	}
	else {
		memset(out_urb->buffer, 0, MAX_PACKET_SIZE);
	}

	if (do_period_elapsed) {
		spin_unlock_irqrestore(&sub->lock, lock_flags); // unlock
		snd_pcm_period_elapsed(sub->instance);
		spin_lock_irqsave(&sub->lock, lock_flags);
	}

	if (sub->instance->runtime->period_size > 390) {
		printk("WARNING: Period Size = %d\n", sub->instance->runtime->period_size);
	}
	
	out_urb->instance.transfer_buffer_length = sinn7_framecount_to_buffersize(sub->instance->runtime->period_size);
	sinn7_frames_to_buffer_ex(out_urb->buffer, sub->instance->runtime->period_size, 2, out_urb->buffer);
	
	ret = usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
	if (ret < 0) {
		printk("usb_submit_urb returned %d\n", ret);
		goto out_fail;
	}

	return;

out_fail:
	rt->panic = true;
	printk("PANIC!\n");
	
}

static int sinn7_pcm_init_urb(struct pcm_urb *urb,
			       struct sinn7_chip *chip,
			       unsigned int ep,
			       void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(MAX_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;
	
	usb_fill_bulk_urb(&urb->instance, chip->dev,
			  usb_sndbulkpipe(chip->dev, ep), (void *)urb->buffer,
			  MAX_PACKET_SIZE, handler, urb);
	init_usb_anchor(&urb->submitted);

	urb->instance.context = (void*)urb;
	return 0;
}

void sinn7_pcm_abort(struct sinn7_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;

	if (rt) {
		printk("State: Shutting down!\n");
		rt->panic = true;

		mutex_lock(&rt->stream_mutex);
		sinn7_pcm_stream_stop(rt);
		mutex_unlock(&rt->stream_mutex);
	}
}

static void sinn7_pcm_destroy(struct sinn7_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	int i;

	for (i = 0; i < PCM_N_URBS; i++)
		kfree(rt->out_urbs[i].buffer);

	kfree(chip->pcm);
	chip->pcm = NULL;
}

static void sinn7_pcm_free(struct snd_pcm *pcm)
{
	struct pcm_runtime *rt = pcm->private_data;

	if (rt)
		sinn7_pcm_destroy(rt->chip);
}

int sinn7_pcm_init(struct sinn7_chip *chip, u8 extra_freq)
{
	int i;
	int ret;
	struct snd_pcm *pcm;
	struct pcm_runtime *rt;

	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;
	if (extra_freq)
		rt->extra_freq = 1;

	init_waitqueue_head(&rt->stream_wait_queue);
	mutex_init(&rt->stream_mutex);
	spin_lock_init(&rt->playback.lock);

	for (i = 0; i < PCM_N_URBS; i++)
		sinn7_pcm_init_urb(&rt->out_urbs[i], chip, OUT_EP,
				    sinn7_pcm_out_urb_handler);

	ret = snd_pcm_new(chip->card, "Stereo USB Audio", 0, 1, 0, &pcm);
	if (ret < 0) {
		kfree(rt);
		dev_err(&chip->dev->dev, "Cannot create pcm instance\n");
		return ret;
	}

	pcm->private_data = rt;
	pcm->private_free = sinn7_pcm_free;

	strlcpy(pcm->name, "Stereo USB Audio", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);

	rt->instance = pcm;

	chip->pcm = rt;
	return 0;
}

void sinn7_timer_interrupt(unsigned long data) {
	uint8_t i;
	struct pcm_runtime *rt;
	struct pcm_substream *sub;
	unsigned long flags;
	
	rt = (struct pcm_runtime *)data;
	sub = &rt->playback;

	spin_lock_irqsave(&sub->lock, flags);
	
	if (sub->active)
	{
		for (i = 0; i < PCM_N_URBS; i++) {
			if (rt->out_urbs[i].instance.complete && !rt->out_urbs[i].instance.hcpriv) {
				rt->out_urbs[i].instance.context = (void*)(&rt->out_urbs[i]);
				sinn7_flush_buffers(&rt->out_urbs[i].instance, &rt->out_urbs[i], rt, flags);
				//sinn7_pcm_out_urb_handler(&rt->out_urbs[i].instance);
				break;
			}
		}
		
	}
	
	if (!rt->panic) {
		mod_timer(rt->timer, jiffies + msecs_to_jiffies(2));
	}
		
	spin_unlock_irqrestore(&sub->lock, flags);
}
