#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "pico/bootrom.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

#include "blaustahl.h"
#include "srwp.h"
#include "fram.h"

#define CMD_TEST	0x00
#define CMD_READ	0x01
#define CMD_WRITE	0x02
#define CMD_SIZE	0x0a

int cdc_read_byte(void) {
    uint8_t buf[1];
    if (tud_cdc_connected() && tud_cdc_available()) {
        uint32_t count = tud_cdc_read(buf, 1);
        if (count) {
            return buf[0];
        } else {
            return EOF;
        }
    } else {
        return EOF;
    }
}

int cdc_read_cmd(void) {
	uint8_t buf[1];
	if (tud_cdc_connected() && tud_cdc_available()) {
		uint32_t count = tud_cdc_read(buf, 1);
		if (count) {
			return (int)buf[0];
        } else {
			return EOF;
        }
	} else {
		return EOF;
	}
}

int cdc_read_addr(void) {
	uint32_t buf[1];
	if (tud_cdc_connected() && tud_cdc_available()) {
		uint32_t count = tud_cdc_read(buf, 4);
		if (count == 4) {
			return (int)buf[0];
        } else {
			return EOF;
        }
	} else {
		return EOF;
	}
}

int cdc_read_len(void) {
	uint32_t buf[1];
	if (tud_cdc_connected() && tud_cdc_available()) {
		uint32_t count = tud_cdc_read(buf, 4);
		if (count == 4) {
			return (int)buf[0];
        } else {
			return EOF;
        }
	} else {
		return EOF;
	}
}

int cdc_read_buf(uint8_t *buf, int len) {
	if (tud_cdc_connected() && tud_cdc_available()) {
        uint32_t count = 0;
        while (count < len) {
            uint32_t read = tud_cdc_read(buf + count, len - count);
            if (read == 0) {
                break;
            }
            count += read;
        }
        return (int)count;
	} else {
        return EOF;
    }
}

void cdc_write_buf(uint8_t *buf, int length) {
	for (int i = 0; i < length; i++)
      tud_cdc_write_char(buf[i]);
	tud_cdc_write_flush();
}

void fram_read_buf(uint8_t *buf, int addr, int len) {
    fram_read((char *)buf, addr, len);
}

void fram_write_buf(uint8_t *buf, int addr, int len) {
    for (int i = 0; i < len; i++) {
        fram_write(addr + i, buf[i]);
    }
}

void cmd_test(void) {
    int len = cdc_read_len();
    uint8_t buf[len];
    cdc_read_buf(buf, len);
    cdc_write_buf(buf, len);
}

void cmd_read(void) {
    int addr = cdc_read_addr();
    int len = cdc_read_len();
    uint8_t buf[len];
    fram_read_buf(buf, addr, len);
    cdc_write_buf(buf, len);
}

void cmd_write(void) {
    int addr = cdc_read_addr();
    int len = cdc_read_len();
    for (int i = 0; i < len; i++) {
        int c = cdc_read_byte();
        if (c == EOF) {
            break;
        }
        fram_write(addr + i, c);
    }
}

void cmd_size(void) {
   uint32_t size = FRAM_SIZE;
   uint8_t buf[4];
   memcpy(buf, &size, 4);
   cdc_write_buf(buf, 4);
}

void srwp(void) {
	blaustahl_led(LED_IDLE);

	int cmd = cdc_read_cmd();

	switch (cmd) {
        case CMD_TEST:
            cmd_test();
            break;

        case CMD_READ:
	        blaustahl_led(LED_READ);
            cmd_read();
            break;

        case CMD_WRITE:
            blaustahl_led(LED_WRITE);
            cmd_write();
            break;

        case CMD_SIZE:
            blaustahl_led(LED_READ);
            cmd_size();
            break;

        default:
            break;
	}
}
