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

#include <config.h>
#include "protocol.h"

static int read_control_async(const struct sr_dev_inst *sdi,
		libusb_transfer_cb_fn cb, enum control_requests reg, uint16_t len);
static int write_control_async(const struct sr_dev_inst *sdi,
		libusb_transfer_cb_fn cb, enum control_requests reg, uint16_t value);
static int write_control(const struct sr_dev_inst *sdi,
		enum control_requests reg, uint16_t value);

SR_PRIV int dds140_open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct drv_context *drvc = sdi->driver->context;
	struct sr_usb_dev_inst *usb;
	struct libusb_device_descriptor des;
	libusb_device **devlist;
	int err = SR_ERR, i;
	char connection_id[64];

	devc = sdi->priv;
	usb = sdi->conn;
	libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
	for (i = 0; devlist[i]; i++) {
		libusb_get_device_descriptor(devlist[i], &des);

		if (des.idVendor != devc->profile->fw_vid
		    || des.idProduct != devc->profile->fw_pid)
			continue;

		if ((sdi->status == SR_ST_INITIALIZING) ||
				(sdi->status == SR_ST_INACTIVE)) {
			/*
			 * Check device by its physical USB bus/port address.
			 */
			if (usb_get_port_path(devlist[i], connection_id, sizeof(connection_id)) < 0)
				continue;

			if (strcmp(sdi->connection_id, connection_id))
				/* This is not the one. */
				continue;
		}

		if (!(err = libusb_open(devlist[i], &usb->devhdl))) {
			if (usb->address == 0xff) {
				/*
				 * First time we touch this device after firmware upload,
				 * so we don't know the address yet.
				 */
				usb->address = libusb_get_device_address(devlist[i]);
			}

			sr_info("Opened device on %d.%d (logical) / "
					"%s (physical) interface %d.",
				usb->bus, usb->address,
				sdi->connection_id, USB_INTERFACE);
			err = SR_OK;
		} else {
			sr_err("Failed to open device: %s.",
			       libusb_error_name(err));
			err = SR_ERR;
		}

		/* If we made it here, we handled the device (somehow). */
		break;
	}

	libusb_free_device_list(devlist, 1);

	return err;
}

SR_PRIV void dds140_close(struct sr_dev_inst *sdi)
{
	struct sr_usb_dev_inst *usb;

	usb = sdi->conn;
	if (!usb->devhdl)
		return;

	sr_info("Closing device on %d.%d (logical) / %s (physical) interface %d.",
		usb->bus, usb->address, sdi->connection_id, USB_INTERFACE);
	libusb_release_interface(usb->devhdl, USB_INTERFACE);
	libusb_close(usb->devhdl);
	usb->devhdl = NULL;
	sdi->status = SR_ST_INACTIVE;
}

static void start_wait_for_fifo(const struct sr_dev_inst *sdi);

libusb_transfer_cb_fn s_cb;
uint32_t s_data_amount;
static void start_data_transfer(const struct sr_dev_inst *sdi) {
	struct sr_usb_dev_inst *usb;
	struct libusb_transfer *transfer;
	int ret;
	unsigned char *buf;

    usb = sdi->conn;
    if (!(buf = g_try_malloc(s_data_amount))) {
        sr_err("Failed to malloc USB endpoint buffer.");
        return;
    }
    transfer = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(
            transfer, usb->devhdl, DDS140_EP_IN, buf, s_data_amount, s_cb, (void *)sdi, 4000);
    if ((ret = libusb_submit_transfer(transfer)) < 0) {
        sr_err("Failed to submit transfer: %s.", libusb_error_name(ret));
        /* TODO: Free them all. */
        libusb_free_transfer(transfer);
        g_free(buf);
    }
}

static void LIBUSB_CALL wait_for_fifo_cb(struct libusb_transfer *trans){
	struct sr_dev_inst *sdi = trans->user_data;
	char data = libusb_control_transfer_get_data(trans)[0];

	g_free(trans->buffer);
	libusb_free_transfer(trans);

	if (0x21 == data) {
		start_data_transfer(sdi);
	} else {
		start_wait_for_fifo(sdi);
	}
}

static void LIBUSB_CALL write_0x33_cb(struct libusb_transfer *trans){ // ? name: start cb?
	struct sr_dev_inst *sdi = trans->user_data;
	g_free(trans->buffer);
	libusb_free_transfer(trans);
	start_wait_for_fifo(sdi);
	//start_data_transfer(sdi);
}

static void start_wait_for_fifo(const struct sr_dev_inst *sdi) {
	read_control_async(sdi, wait_for_fifo_cb, 0x50, 1);
}

SR_PRIV int dds140_get_channeldata(const struct sr_dev_inst *sdi,
		libusb_transfer_cb_fn cb, uint32_t data_amount)
{
	s_cb = cb;
	s_data_amount = data_amount;
	//sr_dbg("Request channel data. %x", sdi);

	write_control_async(sdi, write_0x33_cb, 0x33, 0);
	//start_wait_for_fifo(sdi);

	return SR_OK;
}

static uint8_t samplerate_to_reg(uint64_t samplerate)
{
	const uint64_t samplerate_values[] = {SAMPLERATE_VALUES};
	const uint8_t samplerate_regs[] = {SAMPLERATE_REGS};
	uint32_t i;
	for (i = 0; i < ARRAY_SIZE(samplerate_values); i++) {
		if (samplerate_values[i] == samplerate)
			return samplerate_regs[i];
	}

	sr_err("Failed to convert samplerate: %" PRIu64 ".", samplerate);

	return samplerate_regs[ARRAY_SIZE(samplerate_values) - 1];
}

static uint8_t voltage_to_reg(uint8_t channel, uint8_t state)
{
	const uint8_t vdiv_reg[NUM_CHANNELS][NUM_VDIVS] = {{VDIV_CH1_REG_VAL},{VDIV_CH2_REG_VAL}};
	if (state < ARRAY_SIZE(vdiv_reg[channel])) {
		return vdiv_reg[channel][state];
	} else {
		sr_err("Failed to convert vdiv: %d.", state);
		return vdiv_reg[channel][ARRAY_SIZE(vdiv_reg) - 1];
	}
}

static int read_control(const struct sr_dev_inst *sdi,
		enum control_requests reg, uint8_t *data, uint16_t len)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	int ret;
	if ((ret = libusb_control_transfer(usb->devhdl,
			0x80 | LIBUSB_REQUEST_TYPE_VENDOR, (uint8_t)reg,
			0, 0, data, len, 100)) <= 0) {
		sr_err("Failed to control transfer: 0x%x: %s.", reg,
			libusb_error_name(ret));
		return ret;
	}
	return 0;
}

static int write_control(const struct sr_dev_inst *sdi,
		enum control_requests reg, uint16_t value)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	int ret;
	uint8_t res = 0;

	sr_spew("dds140_write_control: 0x%p 0x%x 0x%x", usb->devhdl, reg, value);

	if ((ret = libusb_control_transfer(usb->devhdl,
			LIBUSB_REQUEST_TYPE_VENDOR, (uint8_t)reg,
			value, 0, &res, 1, 0)) <= 0) {
		sr_err("Failed to control transfer: 0x%x: %s.", reg,
			libusb_error_name(ret));
		return ret;
	}
	return 0;
}

static int write_control_async(const struct sr_dev_inst *sdi,
		libusb_transfer_cb_fn cb, enum control_requests reg, uint16_t value)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	unsigned char *buf = malloc(LIBUSB_CONTROL_SETUP_SIZE + 1);
	struct libusb_transfer *transfer;
	if (!buf)
		return -1;
	
	transfer = libusb_alloc_transfer(0);
	if (!transfer) {
		free(buf);
		return -1;
	}
	libusb_fill_control_setup(buf, 0, reg, value, 0, 1);

	libusb_fill_control_transfer(transfer, usb->devhdl, buf, cb, (void *)sdi,
		1000);
	libusb_submit_transfer(transfer);
	return 0;
}

static int read_control_async(const struct sr_dev_inst *sdi,
		libusb_transfer_cb_fn cb, enum control_requests reg, uint16_t len)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	unsigned char *buf = malloc(LIBUSB_CONTROL_SETUP_SIZE + len);
	struct libusb_transfer *transfer;
	if (!buf)
		return -1;
	
	transfer = libusb_alloc_transfer(0);
	if (!transfer) {
		free(buf);
		return -1;
	}
	libusb_fill_control_setup(buf, 0x80, reg, 0, 0, len);

	libusb_fill_control_transfer(transfer, usb->devhdl, buf, cb, (void *)sdi,
		1000);
	libusb_submit_transfer(transfer);
	return 0;
}

SR_PRIV int dds140_start_data_collecting(const struct sr_dev_inst *sdi)
{
	sr_dbg("trigger");
    write_control(sdi, 0x34, (uint8_t)0x00); //
    write_control(sdi, 0x35, (uint8_t)0x00); //
	//write_control(sdi, 0x33, (uint8_t)0x00); //
	return 0;
}

SR_PRIV int dds140_stop_data_collecting(const struct sr_dev_inst *sdi)
{
	return 0;// TODO write_control(sdi, TRIGGER_REG, 0);
}

SR_PRIV int dds140_update_samplerate(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	sr_dbg("update samplerate %d", samplerate_to_reg(devc->samplerate));

	write_control(sdi, 0x24, 0x18);
	return write_control(sdi, 0x94, samplerate_to_reg(devc->samplerate));
}

SR_PRIV int dds140_update_vdiv(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	int ret1, ret2;

	sr_dbg("update vdiv %d %d", voltage_to_reg(0, devc->voltage[0]),
		voltage_to_reg(1, devc->voltage[1]));

	ret1 = write_control(sdi, VDIV_CH1_REG, voltage_to_reg(0, devc->voltage[0]));
	ret2 = write_control(sdi, VDIV_CH2_REG, voltage_to_reg(1, devc->voltage[1]));

	return MIN(ret1, ret2);
}

SR_PRIV int dds140_update_coupling(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t coupling = 0xFF & ((devc->coupling[1] << 4) | devc->coupling[0]);
	if (devc->has_coupling) {
		sr_dbg("update coupling 0x%x", coupling);
		return write_control(sdi, COUPLING_REG, coupling);
	} else {
		sr_dbg("coupling not supported");
		return SR_OK;
	}
}

SR_PRIV int dds140_update_channels(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t chan = devc->ch_enabled[1] ? 2 : 1;
	sr_dbg("update channels amount %d", chan);

	return write_control(sdi, CHANNELS_REG, chan);
}

static void signal_generator(const struct sr_dev_inst *sdi) {
	write_control(sdi, 0x70, 0x55d6);
	write_control(sdi, 0x71, 0x4000);
	write_control(sdi, 0x72, 0x95d6);
	write_control(sdi, 0x73, 0x8000);
	write_control(sdi, 0x74, 0x0000);
	write_control(sdi, 0x76, 0x00fc);
	write_control(sdi, 0x77, 0x00d7);
	write_control(sdi, 0x78, 0x00fe);
	write_control(sdi, 0x79, 0x0079);
	write_control(sdi, 0x7a, 0x00fb);
	write_control(sdi, 0x7b, 0x005c);
	write_control(sdi, 0x7c, 0x00ff);
	write_control(sdi, 0x7d, 0x00f4);
	write_control(sdi, 0x63, 0x0000);
}

SR_PRIV int dds140_init(const struct sr_dev_inst *sdi)
{
	sr_dbg("Initializing");

	/* ++++++ Ported from Logic 140 ++++++++++ */
    write_control(sdi, 0x76, (uint8_t)0xe8); // timers
    write_control(sdi, 0x77, (uint8_t)0x9b); // timers
    write_control(sdi, 0x78, (uint8_t)0xe8); // timers
    write_control(sdi, 0x79, (uint8_t)0x9b); // timers
    write_control(sdi, 0x63, (uint8_t)0x04); //
    write_control(sdi, 0x75, (uint8_t)0x00); // timers
    write_control(sdi, 0x34, (uint8_t)0x00);
    write_control(sdi, 0x34, (uint8_t)0x00);
    write_control(sdi, 0x7a, (uint8_t)0xfb); // timers
    write_control(sdi, 0x7b, (uint8_t)0x8c); // timers
    write_control(sdi, 0x7c, (uint8_t)0xff); // timers
    write_control(sdi, 0x7d, (uint8_t)0xc4); // timers
    write_control(sdi, 0x24, (uint8_t)0x10); //
    write_control(sdi, 0x94, (uint8_t)0x1c); // 10mhz
    write_control(sdi, 0x22, (uint8_t)0x00); // voltage ch1
    write_control(sdi, 0x24, (uint8_t)0x18); //
    write_control(sdi, 0x23, (uint8_t)0x00); // voltage ch2
    write_control(sdi, 0x24, (uint8_t)0x18); //
    write_control(sdi, 0x94, (uint8_t)0x1c); // 10mhz
    write_control(sdi, 0x24, (uint8_t)0x18); // enable ch1 & ch2
    write_control(sdi, 0xe7, (uint8_t)0x00); //
	/* ------ Ported from Logic 140 ---------- */

	dds140_update_samplerate(sdi);
	dds140_update_vdiv(sdi);
	//dds140_update_coupling(sdi);
	// dds140_update_channels(sdi); /* Only 2 channel mode supported. */

	//signal_generator(sdi);

	return SR_OK;
}
