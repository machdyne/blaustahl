/* 
 * RP2040 FRAM driver (MB85RS64PNF)
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 */

#include "pico/stdlib.h"
#include "hardware/spi.h"

#include "blaustahl.h"
#include "fram.h"

void fram_init(void) {

	gpio_init(BS_FRAM_SS);
	gpio_init(BS_FRAM_MISO);
	gpio_init(BS_FRAM_MOSI);
	gpio_init(BS_FRAM_SCK);

	gpio_set_function(BS_FRAM_MISO, GPIO_FUNC_SPI);
	gpio_set_function(BS_FRAM_MOSI, GPIO_FUNC_SPI);
	gpio_set_function(BS_FRAM_SCK, GPIO_FUNC_SPI);

	gpio_set_dir(BS_FRAM_SS, 1);
	gpio_set_dir(BS_FRAM_MOSI, 1);
	gpio_set_dir(BS_FRAM_SCK, 1);

	spi_init(BS_FRAM_SPI, 10000 * 1000);	// 10 MHz

}

void fram_read(char *buf, int addr, int len) {

	int i;
	uint8_t cmdbuf[3] = { 0x03, addr >> 8, addr & 0xff };	// READ

	gpio_put(BS_FRAM_SS, 0);
	spi_write_blocking(BS_FRAM_SPI, cmdbuf, 3);
	spi_read_blocking(BS_FRAM_SPI, 0x00, buf, len);
	gpio_put(BS_FRAM_SS, 1);

}

void fram_write_enable(void) {

	uint8_t cmdbuf[1] = { 0x06 };

	gpio_put(BS_FRAM_SS, 0);
	spi_write_blocking(BS_FRAM_SPI, cmdbuf, 1); // WREN
	gpio_put(BS_FRAM_SS, 1);

}

void fram_write_byte(int addr, unsigned char d) {

	uint8_t cmdbuf[4] = { 0x02, addr >> 8, addr & 0xff, d };	// WRITE

	fram_write_enable(); // auto-disabled after each write

	gpio_put(BS_FRAM_SS, 0);
	spi_write_blocking(BS_FRAM_SPI, cmdbuf, 4);
	gpio_put(BS_FRAM_SS, 1);

}

void fram_write(int addr, char *buf, int len) {

	uint8_t cmdbuf[3] = { 0x02, addr >> 8, addr & 0xff };	// WRITE

	fram_write_enable(); // auto-disabled after each write

	gpio_put(BS_FRAM_SS, 0);
	spi_write_blocking(BS_FRAM_SPI, cmdbuf, 3);
	spi_write_blocking(BS_FRAM_SPI, buf, len);
	gpio_put(BS_FRAM_SS, 1);

}
