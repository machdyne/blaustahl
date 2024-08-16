/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BLAUSTAHL_H_
#define BLAUSTAHL_H_

#define BLAUSTAHL_VERSION "0.1.0"

int cdc_getchar(void);
void cdc_print(const char *str);
void cdc_printf(const char *fmt, ...);
void cdc_putchar(const char ch);

void blaustahl_led(uint16_t intensity);
void blaustahl_dfu(void);

// USB VENDOR CLASS COMMANDS

#define BS_CMD_NOP			0x00
#define BS_CMD_WRITE_BYTE	0x21	// <cmd> <addr:16> <data:8>
#define BS_CMD_READ_BYTE	0x31	// <cmd> <addr:16>

// PINS

#define BS_LED 			9

#define BS_FRAM_SPI		spi1
#define BS_FRAM_MOSI		11
#define BS_FRAM_MISO		12
#define BS_FRAM_SS		13
#define BS_FRAM_SCK		14

// LED PWM SETTINGS

#define LED_STARTUP 100
#define LED_IDLE 0
#define LED_READ 100
#define LED_WRITE 250

#endif
