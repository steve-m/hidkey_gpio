/*
 * hidkey_gpio
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * 
 * Using a HID keyboard as general purpose input/outputs
 * http://wiki.steve-m.de/hidkey_gpio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 *(at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string.h>

#include <ctype.h>

#include <libusb.h>
#include <pthread.h>

#define DEBUG
#ifdef DEBUG
#define DEBUGP(...)	fprintf(stderr, __VA_ARGS__)
#else
#define DEBUGP(...)
#endif

static struct libusb_device_handle *devh = NULL;
static int do_exit = 0;
static uint8_t input_state = 0;

/* delay until a 'keypress' has been detected by the keyboard in the
 * worst case, highly device dependent, YMMV */
#define INPUT_POLL_DELAY	40000
#define INPUT_INTERRUPT_DELAY	50000
//#define USE_INTERRUPT		/* slower in my case */

#define OUTPUT_DELAY		500

/* HID protocol constants */
#define HID_REPORT_GET		0x01
#define HID_REPORT_SET		0x09

#define HID_GET_IDLE		0x02
#define HID_SET_IDLE		0x0a
#define HID_SET_PROTOCOL	0x0b

#define HID_INPUT		0x0100
#define HID_OUTPUT		0x0200
#define INFINITE_IDLE		0x0000

static int hidkey_find_device(void)
{
	uint16_t usbvid = 0x045e, usbpid = 0x0750;

	devh = libusb_open_device_with_vid_pid(NULL, usbvid, usbpid);
	if (devh > 0) {
		DEBUGP("[hidkey] opened HID keyboard\n");

		if (libusb_kernel_driver_active(devh, 0) == 1) {
			DEBUGP("detaching kernel driver for iface 0\n");
			libusb_detach_kernel_driver(devh, 0);
		}

		if (libusb_kernel_driver_active(devh, 1) == 1) {
			DEBUGP("detaching kernel driver for iface 1\n");
			libusb_detach_kernel_driver(devh, 1);
		}

		return 0;
	}

	return -EIO;
}

static pthread_t poll_thread;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t exit_cond_lock = PTHREAD_MUTEX_INITIALIZER;

static void request_exit(int code)
{
	do_exit = code;
	pthread_cond_signal(&exit_cond);
}

static void *poll_thread_main(void *arg)
{
	int r = 0;
	DEBUGP("poll thread running\n");

	while (!do_exit) {
		struct timeval tv = { 1, 0 };
		r = libusb_handle_events_timeout(NULL, &tv);
		if (r < 0) {
			pthread_cond_signal(&exit_cond);
			break;
		}
	}

	DEBUGP("poll thread shutting down\n");
	return NULL;
}

#define INTR_LENGTH		8
#define EP_INTR			0x81
static struct libusb_transfer *irq_transfer = NULL;
static uint8_t irqbuf[INTR_LENGTH];

static void LIBUSB_CALL irq_cb(struct libusb_transfer *transfer)
{
	input_state = transfer->buffer[0];

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		DEBUGP(stderr, "irq transfer status %d?\n", transfer->status);
		do_exit = 2;
		libusb_free_transfer(transfer);
		irq_transfer = NULL;
		return;
	}

	fprintf(stderr, "IRQ callback %02x\n", transfer->buffer[0]);

	if (libusb_submit_transfer(irq_transfer) < 0) {
		fprintf(stderr, "error submitting interrupt transfer!\n");
		do_exit = 2;
	}
}

static int hidkey_alloc_transfers(void)
{
	irq_transfer = libusb_alloc_transfer(0);
	if (!irq_transfer)
		return -ENOMEM;

	libusb_fill_interrupt_transfer(irq_transfer, devh, EP_INTR, irqbuf,
		sizeof(irqbuf), irq_cb, NULL, 0);

	return 0;
}

static int hidkey_setpin(int pin, int value)
{
	int r;
	static uint8_t data = 0x00;
	int invert = 1;	//pin & PIN_INVERSE;

	if ((pin & ~invert) > 3)
		return -1;

	if (!!value ^ !!invert)
		data &= ~(1 << (pin-1));
	else
		data |= (1 << (pin-1));

	usleep(OUTPUT_DELAY);
	r = libusb_control_transfer(devh, 0x21, HID_REPORT_SET,
				    HID_OUTPUT, 0x00, &data, 1, 0);

	DEBUGP("%s: pin %i, val %i, ret: %i\n", __FUNCTION__, pin, value, r);

	return r;
}

static int hidkey_getpin(int pin)
{
	int r, val;
	uint8_t data[8];
	uint8_t *modifier_keys;
	int invert = 0;//pin & PIN_INVERSE;

	/* TODO cache outputs and return them if needed */
	if ((pin & ~invert) < 4)
		return -1;

#ifdef USE_INTERRUPT
	usleep(INPUT_INTERRUPT_DELAY);
	modifier_keys = &input_state;
#else
	usleep(INPUT_POLL_DELAY);
	r = libusb_control_transfer(devh, 0x21 | 0x80, HID_REPORT_GET,
				    HID_INPUT, 0x00, data, 8, 0);

	modifier_keys = &data[0];
#endif

	val = (*modifier_keys & (1 << (pin-4))) ? 1 : 0;

	DEBUGP("%s: pin %i, val %i\n", __FUNCTION__, pin, val);

	return (invert) ? !val : val;
}
int main(int argc, char **argv)
{
	int r, opt;
#if 0
	while ((opt = getopt(argc, argv, "f:")) != -1) {
		switch (opt) {
		case 'f':
			break;
		default:
			break;
		}
	}
#endif
	r = libusb_init(NULL);
	if (r < 0) {
		fprintf(stderr, "Failed to initialize libusb\n");
		exit(1);
	}

	r = hidkey_find_device();
	if (r < 0) {
		fprintf(stderr, "Could not find/open device\n");
		goto out;
	}

	r = libusb_claim_interface(devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		goto out;
	}

	r = pthread_create(&poll_thread, NULL, poll_thread_main, NULL);
	if (r)
		goto out_deinit;

	r = hidkey_alloc_transfers();
	if (r < 0)
		goto out_deinit;

	r = libusb_submit_transfer(irq_transfer);

	while (!do_exit) {
		hidkey_setpin(3, 0);
		hidkey_setpin(1, 1);
		usleep(500000);
		hidkey_setpin(1, 0);
		hidkey_setpin(2, 1);
		usleep(500000);
		hidkey_setpin(2, 0);
		hidkey_setpin(3, 1);
		usleep(500000);
	}

	r = libusb_cancel_transfer(irq_transfer);

out_deinit:
	printf("shutting down...\n");
	pthread_join(poll_thread, NULL);

	libusb_release_interface(devh, 0);

out:
	libusb_close(devh);
	libusb_exit(NULL);
	return r >= 0 ? r : -r;
}

