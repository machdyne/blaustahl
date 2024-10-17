// Simple Read Write Protocol (SRWP) for FRAM
/*
Test: 0, 0, len:int, data:byte[]
Read: 0, 1, addr:int, len:int
Write: 0, 2, addr:int, len:int, data:byte[]
*/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "pico/bootrom.h"
#include "tusb.h"

#include "blaustahl.h"
#include "fram.h"

#define CMD_READ 1
#define CMD_WRITE 2

int cdc_read_cmd(void);
int cdc_read_addr(void);
int cdc_read_len(void);
int cdc_read_buf(uint8_t *buf, int len);
void cdc_write_buf(uint8_t *buf, int length);
void fram_read_buf(uint8_t *buf, int addr, int len);
void fram_write_buf(uint8_t *buf, int addr, int len);
void srwp(void);

