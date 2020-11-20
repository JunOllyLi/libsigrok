/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2015 Christer Ekholm <christerekholm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROK_HARDWARE_DDS140_PROTOCOL_H
#define LIBSIGROK_HARDWARE_DDS140_PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "DDS140"

#define MAX_RENUM_DELAY_MS	3000

#define DEFAULT_VOLTAGE		2
#define DEFAULT_COUPLING	COUPLING_DC
#define DEFAULT_SAMPLERATE	SR_KHZ(39)

#define NUM_CHANNELS		2

#define SAMPLERATE_VALUES \
	SR_MHZ(100), SR_MHZ(80), SR_MHZ(10), \
	SR_KHZ(625), SR_KHZ(39),

#define SAMPLERATE_REGS \
	0x10, 0x11, 0x1c, 0x18, 0x1b,

//22,08 50mV
//22,04 100mV
//22,00 200mV
//22,06 500mV
//22,02 1V
//22,02 2V
//22,02 5V
//
//CH2 Voltage setting:
//23,20 50mV
//23,10 100mV
//23,00 200mV
//23,12 500mV
//23,02 1V
//23,02 2V
//23,02 5V
#define VDIV_VALUES \
	{ 50, 1000 }, \
	{ 100, 1000 }, \
	{ 200, 1000 }, \
	{ 500, 1000 }, \
	{ 1, 1 },

#define VDIV_CH1_REG_VAL \
	0x08, 0x04, 0x00, 0x06, 0x02,

#define VDIV_CH2_REG_VAL \
	0x20, 0x10, 0x00, 0x12, 0x02,

#define NUM_VDIVS 5

#define VDIV_MULTIPLIER		10

/* Weird flushing needed for filtering glitch away. */
#define FLUSH_PACKET_SIZE	1024

#define MIN_PACKET_SIZE		512
#ifdef _WIN32
#define MAX_PACKET_SIZE		(2 * 1024 * 1024)
#else
#define MAX_PACKET_SIZE		(12 * 1024 * 1024)
#endif

#define DDS140_EP_IN		0x82
#define USB_INTERFACE		0
#define USB_CONFIGURATION	1

enum control_requests {
	VDIV_CH1_REG   = 0x22,
	VDIV_CH2_REG   = 0x23,
	SAMPLERATE_REG = 0,
	TRIGGER_REG    = 0,
	CHANNELS_REG   = 0,
	COUPLING_REG   = 0,
};

enum states {
	IDLE,
	FLUSH,
	CAPTURE,
	STOPPING,
};

enum couplings {
	COUPLING_AC = 0,
	COUPLING_DC,
};

struct dds140_profile {
	/* VID/PID after cold boot */
	uint16_t orig_vid;
	uint16_t orig_pid;
	/* VID/PID after firmware upload */
	uint16_t fw_vid;
	uint16_t fw_pid;
	uint16_t fw_prod_ver;
	const char *vendor;
	const char *model;
	const char *firmware;
	const char **coupling_vals;
	uint8_t coupling_tab_size;
	gboolean has_coupling;
};

struct dev_context {
	const struct dds140_profile *profile;
	GSList *enabled_channels;
	/*
	 * We can't keep track of an FX2-based device after upgrading
	 * the firmware (it re-enumerates into a different device address
	 * after the upgrade) this is like a global lock. No device will open
	 * until a proper delay after the last device was upgraded.
	 */
	int64_t fw_updated;
	int dev_state;
	uint64_t samp_received;
	uint64_t aq_started;

	uint64_t read_start_ts;

	gboolean ch_enabled[NUM_CHANNELS];
	int voltage[NUM_CHANNELS];
	int coupling[NUM_CHANNELS];
	const char **coupling_vals;
	uint8_t coupling_tab_size;
	gboolean has_coupling;
	uint64_t samplerate;

	uint64_t limit_msec;
	uint64_t limit_samples;
};

SR_PRIV int dds140_open(struct sr_dev_inst *sdi);
SR_PRIV void dds140_close(struct sr_dev_inst *sdi);
SR_PRIV int dds140_get_channeldata(const struct sr_dev_inst *sdi,
		libusb_transfer_cb_fn cb, uint32_t data_amount);

SR_PRIV int dds140_start_data_collecting(const struct sr_dev_inst *sdi);
SR_PRIV int dds140_stop_data_collecting(const struct sr_dev_inst *sdi);

SR_PRIV int dds140_update_coupling(const struct sr_dev_inst *sdi);
SR_PRIV int dds140_update_samplerate(const struct sr_dev_inst *sdi);
SR_PRIV int dds140_update_vdiv(const struct sr_dev_inst *sdi);
SR_PRIV int dds140_update_channels(const struct sr_dev_inst *sdi);
SR_PRIV int dds140_init(const struct sr_dev_inst *sdi);

#endif
