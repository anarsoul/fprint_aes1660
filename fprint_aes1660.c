/*
 * fprint_aes1660 driver prototype
 * Copyright (c) 2012 Vasily Khoruzhick <anarsoul@gmail.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <libusb.h>

#include "aes1660.h"

#define EP_IN (1 | LIBUSB_ENDPOINT_IN)
#define EP_OUT (2 | LIBUSB_ENDPOINT_OUT)
#define BULK_TIMEOUT 4000

#define ARRAY_SIZE(a) (sizeof(a) / (sizeof(*a)))

int aborted = 0;

static void __msg(FILE *stream, const char *msg, va_list args)
{
	vfprintf(stream, msg, args);
	fprintf(stream, "\n");
}

static void die(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	__msg(stderr, msg, args);
	va_end(args);
	exit(1);
}

static void msg(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	__msg(stdout, msg, args);
	va_end(args);
}

static void msg_err(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	__msg(stderr, msg, args);
	va_end(args);
}

static void sighandler(int num)
{
	msg("got signal %d\n", num);
	aborted = 1;
}

static int aes1660_cmd_write(libusb_device_handle *h, unsigned char cmd)
{
	int r;
	int actual_len = 0;

	r = libusb_bulk_transfer(h, EP_OUT, &cmd, 1, &actual_len, BULK_TIMEOUT);

	if (!r && (actual_len != 1))
		return -EIO;

	return r;
}

static int aes1660_reg_write(libusb_device_handle *h, unsigned char reg, unsigned char data)
{
	int r;
	int actual_len = 0;
	unsigned char cmd_data[2];

	cmd_data[0] = reg;
	cmd_data[1] = data;

	r = libusb_bulk_transfer(h, EP_OUT, cmd_data, 2, &actual_len, BULK_TIMEOUT);

	if (!r && (actual_len != 2))
		return -EIO;

	return r;
}

static int do_aes1660_cmd(libusb_device_handle *h, const unsigned char *cmd, size_t len)
{
	int r, actual_len;

	msg("Sending cmd %p, len %d\n", cmd, len);
	r = libusb_bulk_transfer(h, EP_OUT, (unsigned char *)cmd, len, &actual_len, BULK_TIMEOUT);
	if ((r < 0) || (actual_len != len)) {
		msg_err("Failed to send CMD, len is %d, %d, ret: %d!\n", len, actual_len, r);
		return r;
	}
	return 0;
}

static int read_aes1660_response(libusb_device_handle *h, unsigned char *cmd_res, size_t res_size)
{
	int r, actual_len, i;
	r = libusb_bulk_transfer(h, EP_IN, cmd_res, res_size, &actual_len, BULK_TIMEOUT);
	if (r < 0) {
		msg_err("Failed to receive response!");
		return r;
	}
	msg("Received: %d bytes\n", actual_len);
	if (res_size != actual_len) {
		msg("Unexpected response, waited for %d bytes, got %d\n",
			res_size, actual_len);
		return -EIO;
	}
	for (i = 0; i < actual_len; i++) {
		printf("%.2x ", cmd_res[i]);
		if ((i % 8) == 7)
			printf("\n");
	}
	printf("\n---\n");
	return 0;
}

/* Expects 583-byte chunk */
static int image_sum(unsigned char *buf)
{
	int sum = 0, x, y;
	int offset = 42;
	for (y = 0; y < 128; y++) {
		for (x = 0; x < 4; x++) {
			sum += (int)(buf[offset + y * 4 + x] >> 4);
			sum += (int)(buf[offset + y * 4 + x] & 0xf);
		}
	}
	
	msg("Image sum is %d\n", sum);
	return sum;
}

/* Expects 583-byte chunk */
static void write_ppm(FILE *out, unsigned char *buf)
{
	int offset = 42, x, y;
	fprintf(out, "P2\n");
	fprintf(out, "8 128\n");
	fprintf(out, "15\n");

	for (y = 0; y < 128; y++) {
		for (x = 0; x < 4; x++) {
			fprintf(out, "%.2d %.2d ", (int)(buf[offset + y * 4 + x] >> 4), (int)(buf[offset + y * 4 + x] & 0xf));
		}
		fprintf(out, "\n");
	}
}

static int aes1660_test(libusb_device_handle *h)
{
	int r, i, idx = 0;
	char filename[128];
	FILE *out;
	unsigned char cmd_res[4096];

	r = do_aes1660_cmd(h, set_idle_cmd, sizeof(set_idle_cmd));
	if (r) {
		return r;
	}

	r = do_aes1660_cmd(h, read_id_cmd, sizeof(read_id_cmd));
	if (r) {
		return r;
	}

	/* Get ID response */
	r = read_aes1660_response(h, cmd_res, 8);
	if (r) {
		return r;
	}
	if (cmd_res[0] != 0x07) {
		msg("Bogus response instead of ID, type: %d\n", cmd_res[0]);
	}

	msg("Sensor device id: %.2x%2x, bcdDevice: %.2x.%.2x, init status: %.2x\n",
		cmd_res[4], cmd_res[3], cmd_res[5], cmd_res[6], cmd_res[7]);

	/* Need to perform long init */
	if (cmd_res[7] == 0x00) {
		msg("Performing long init...\n");
		for (i = 0; i < ARRAY_SIZE(init_cmds); i++) {
			r = do_aes1660_cmd(h, init_cmds[i].cmd, init_cmds[i].len);
			if (r) {
				return r;
			}
			r = read_aes1660_response(h, cmd_res, 4);
			if (cmd_res[0] != 0x42) {
				msg("Bogus response instead of 0x42, type: %d\n", cmd_res[0]);
			}
		}
	} else {
		msg("No need in long init...\n");
	}

	/* Read ID again... */
	r = do_aes1660_cmd(h, read_id_cmd, sizeof(read_id_cmd));
	if (r) {
		return r;
	}

	/* Get ID response */
	r = read_aes1660_response(h, cmd_res, 8);
	if (r) {
		return r;
	}
	if (cmd_res[0] != 0x07) {
		msg_err("Bogus response instead of ID, type: %d\n", cmd_res[0]);
	}
	msg("Sensor device id: %.2x%2x, bcdDevice: %.2x.%.2x, init status: %.2x\n",
		cmd_res[4], cmd_res[3], cmd_res[5], cmd_res[6], cmd_res[7]);

	/* Do calibrate */
	r = do_aes1660_cmd(h, calibrate_cmd, sizeof(calibrate_cmd));
	if (r) {
		return r;
	}

	/* Get calibrate response */
	r = read_aes1660_response(h, cmd_res, 4);
	if (r) {
		return r;
	}
	if (cmd_res[0] != 0x06) {
		msg_err("Bogus response instead of 0x06, type: %d\n", cmd_res[0]);
	}

	r = do_aes1660_cmd(h, led_blink_cmd, sizeof(led_blink_cmd));
	if (r) {
		return r;
	}
	/* Wait for finger... */
	do {
		r = do_aes1660_cmd(h, finger_det_cmd, sizeof(finger_det_cmd));
		if (r) {
			return r;
		}

		r = read_aes1660_response(h, cmd_res, 4);
		if (r) {
			return r;
		}
		if (cmd_res[0] != 0x01) {
			msg_err("Bogus finger det response %.2x!\n", cmd_res[0]);
		}
	} while (cmd_res[3] != 0x01 && !aborted);

	msg("Finger detected!");

	/* Do calibrate */
	r = do_aes1660_cmd(h, calibrate_cmd, sizeof(calibrate_cmd));
	if (r) {
		return r;
	}

	/* Get calibrate response */
	r = read_aes1660_response(h, cmd_res, 4);
	if (r) {
		return r;
	}
	if (cmd_res[0] != 0x06) {
		msg_err("Bogus response instead of 0x06, type: %d\n", cmd_res[0]);
	}

	/* Wait for finger... */
	do {
		r = do_aes1660_cmd(h, capture_cmd, sizeof(capture_cmd));
		if (r) {
			return r;
		}

		r = read_aes1660_response(h, cmd_res, 583);
		if (r) {
			return r;
		}
		if (cmd_res[0] != 0x49) {
			msg_err("Bogus capture response %.2x!\n", cmd_res[0]);
		}
		snprintf(filename, sizeof(filename), "frame-%.5d.pnm", idx++);
		out = fopen(filename, "wb");
		if (out) {
			write_ppm(out, cmd_res);
			fclose(out);
		}

	} while ((image_sum(cmd_res) > 100) && !aborted);

	msg("Done!\n");

	r = do_aes1660_cmd(h, set_idle_cmd, sizeof(set_idle_cmd));
	if (r) {
		return r;
	}

	msg("Probed device successfully!\n");

	return 0;
}

int main(int argc, char *argv[])
{
	int r;
	libusb_device_handle *h;
	libusb_context *ctx;

	struct sigaction sigact;

        sigact.sa_handler = sighandler;
        sigemptyset(&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction(SIGINT, &sigact, NULL);
        sigaction(SIGTERM, &sigact, NULL);
        sigaction(SIGQUIT, &sigact, NULL);

	libusb_init(&ctx);

	h = libusb_open_device_with_vid_pid(ctx, 0x08ff, 0x1660);
	if (!h) {
		msg_err("Can't open aes1660 device!\n");
		return 0;
	}

	r = libusb_claim_interface(h, 0);
	if (r < 0) {
		msg_err("Failed to claim interface 0\n");
		goto exit_closelibusb;
	}

	r = aes1660_test(h);
	if (r < 0) {
		msg_err("Failed to probe aes1660\n");
		goto exit_closelibusb;
	}

exit_closelibusb:
	libusb_close(h);
	libusb_exit(ctx);

	return 0;

}
