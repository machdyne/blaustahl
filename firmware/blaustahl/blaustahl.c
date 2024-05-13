/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2024 Lone Dynamics Corporation <info@lonedynamics.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Blaustahl Firmware (work-in-progress)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <string.h>

// Pico
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "pico/binary_info.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"

#include "hardware/pwm.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/clocks.h"

#include "tusb.h"

#include "blaustahl.h"
#include "editor.h"
#include "fram.h"
#include "cdc_out.h"

void core1_main(void);

void init_blaustahl(void);

void blaustahl_task(void);

void cdc_print(const char *str) {
	cdc_usb_out_chars(str, strlen(str));
	tud_cdc_write_flush();
}

void cdc_printf(const char *fmt, ...) {
   char buf[1024];
   va_list args;
   va_start(args, fmt);
   vsprintf(buf, fmt, args);
   cdc_print(buf);
   va_end(args);
}

void cdc_putchar(const char ch) {
	tud_cdc_write_char(ch);
	tud_cdc_write_flush();
}

void blaustahl_task() {

	uint8_t buf[64];

	if (!tud_vendor_available()) return;

	uint32_t len = tud_vendor_read(buf, 64);

	/*
	char tmp[256];
	sprintf(tmp, "RX %d bytes from host\r\n", len);
	tud_cdc_write_str(tmp);
	sprintf(tmp, "[%x %x %x %x]\r\n", buf[0], buf[1], buf[2], buf[3]);
	tud_cdc_write_str(tmp);
	tud_cdc_write_flush();
	*/

	if (buf[0] == BS_CMD_NOP) {
	}

	if (buf[0] == BS_CMD_READ) {
	}

	if (buf[0] == BS_CMD_WRITE) {
	}

}

void init_blaustahl(void) {

	cdc_printf("init_blaustahl\r\n");

	// init LED
	gpio_init(BS_LED);
	gpio_set_dir(BS_LED, 1);
	gpio_set_function(BS_LED, GPIO_FUNC_PWM);
	gpio_set_outover(BS_LED, GPIO_OVERRIDE_INVERT);

	uint slice_num = pwm_gpio_to_slice_num(BS_LED);
	pwm_set_wrap(slice_num, 499);
	pwm_set_chan_level(slice_num, BS_LED, LED_STARTUP);
	//pwm_set_clkdiv(slice_num, 4);
	pwm_set_clkdiv_int_frac(slice_num, 250, 0);
	pwm_set_enabled(slice_num, true);

	// init FRAM
	fram_init();

}

int main(void) {

	// set the sys clock to 120mhz
	set_sys_clock_khz(120000, true);

	// init tinyusb
	tud_init(BOARD_TUD_RHPORT);

	// init hardware
	init_blaustahl();

	// start editor on second core
   multicore_reset_core1();
   multicore_launch_core1(core1_main);

	// handle USB tasks
	while (1) {

		tight_loop_contents();
		tud_task();
		blaustahl_task();

	}

	return 0;

}

bool init_done = false;

void core1_main(void) {

	sleep_ms(10);

	while (true) {
		if (!init_done && tud_cdc_connected()) {
			init_done = true;
			editor_init();
		} else {
			editor_yield();
		}
	}

}

int cdc_getchar(void) {
	uint8_t buf[1];
	if (tud_cdc_connected() && tud_cdc_available()) {
		uint32_t count = tud_cdc_read(buf, 1);
		if (count)
			return((int)buf[0]);
		else
			return(EOF);
	} else {
		return(EOF);
	}
}

// control LED
void blaustahl_led(uint16_t intensity) {
	pwm_set_gpio_level(BS_LED, intensity);
}

// enter DFU mode
void blaustahl_dfu(void) {
	reset_usb_boot(0, 0);
}
