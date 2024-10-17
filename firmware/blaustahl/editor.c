/*
 * Text Editor for Blaustahl
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "blaustahl.h"
#include "editor.h"
#include "fram.h"

#define ROWS 24
#define COLS 80
#define PAGE_SIZE (ROWS * COLS)

#define VT100_CURSOR_UP			"\e[A"
#define VT100_CURSOR_DOWN		"\e[B"
#define VT100_CURSOR_RIGHT		"\e[C"
#define VT100_CURSOR_LEFT		"\e[D"
#define VT100_CURSOR_HOME		"\e[;H"
#define VT100_CURSOR_MOVE_TO	"\e[%i;%iH"
#define VT100_CURSOR_CRLF		"\e[E"
#define VT100_CLEAR_HOME		"\e[;H"
#define VT100_ERASE_SCREEN		"\e[J"
#define VT100_ERASE_LINE		"\e[K"

#define CH_SOH		0x01	// CTRL-A
#define CH_ENQ		0x05	// CTRL-E
#define CH_ACK		0x06
#define CH_BEL		0x07
#define CH_BS		0x08
#define CH_LF		0x0a
#define CH_CR		0x0d
#define CH_FF		0x0c
#define CH_DLE		0x10
#define CH_DC1		0x11
#define CH_DC3		0x13
#define CH_ETB		0x17
#define CH_CAN		0x18
#define CH_EM		0x19
#define CH_ESC		0x1b
#define CH_DEL		0x7f

#define MODE_EDIT 1
#define MODE_HELP 2

#define STATE_NONE 0
#define STATE_DONE 1
#define STATE_ESC0 2
#define STATE_ESC1 3
#define STATE_ESC2 4

#define KEY_SIZE 256

const char blaustahl_banner[] =
	"BLAUSTAHL FIRMWARE V%s %s\r\n"
	"Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.\r\n"
	"\r\n";

const char help_password[] =
	"\r\n"
	"If you set a password, all text will be decrypted using that\r\n"
	"password and new text will be encrypted using that password.\r\n"
	"\r\n"
	"When you reconnect the device, press CTRL-P again and re-enter\r\n"
	"your password. Press ENTER to use no encryption/decryption.\r\n"
	"\r\n";

const char help_editor[] =
	"COMMANDS:\r\n"
	"\r\n"
	"CTRL-G    HELP\r\n"
	"CTRL-L    REFRESH SCREEN\r\n"
	"CTRL-W    TOGGLE WRITE MODE\r\n"
	"CTRL-S/Q  TOGGLE STATUS BAR\r\n"
//	"CTRL-P    SET ENCRYPTION PASSWORD\r\n"
	"CTRL-Y    ENTER FIRMWARE UPDATE MODE\r\n";

const char help_press_any_key[] =
	"\r\n"
	"Press any key to return to the editor.\r\n";

uint8_t led = LED_IDLE;

void editor(void);
void editor_set_password(void);
void readline(char *buf, int maxlen);
uint32_t xorshift32(uint32_t state[]);
char editor_encode(int addr, char pc);
char editor_decode(int addr, char cc);
void editor_help(void);

#ifdef EDITOR_MAIN
int main(int argc, char *argv[]) {

	editor_init();
	while (1) editor_yield();

	return 0;

}
#endif

int mode = MODE_EDIT;
int state = STATE_NONE;
int page, x, y;

bool write_enabled = false;
bool encryption_enabled = false;
bool status_enabled = true;
uint8_t key[KEY_SIZE];

void editor_init(void) {

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);

	page = 1;
	x = 1;
	y = 1;

	editor_redraw();

}

void editor_redraw(void) {
	char buf[COLS];
	editor_status();
	printf(VT100_CURSOR_MOVE_TO, 24, 59);
	printf("PRESS CTRL-G FOR HELP");

	blaustahl_led(LED_READ);

	for (int y = 0; y < ROWS; y++) {
		if (status_enabled && y == ROWS - 1) continue;
		fram_read(buf, ((page - 1) * PAGE_SIZE) + (y * COLS), COLS);
		printf(VT100_CURSOR_MOVE_TO, y + 1, 1);
		for (int x = 0; x < COLS; x++) {
			char pc = editor_decode(y * COLS + x, buf[x]);
			if (pc > 0x1f && pc < 0x7f)
				cdc_putchar(pc);
			else
				cdc_putchar('.');
		}
	}

	printf(VT100_CURSOR_MOVE_TO, y, x);
	fflush(stdout);
}

void editor_status(void) {
	if (!status_enabled) return;
	printf(VT100_CURSOR_MOVE_TO, 24, 1);
	printf("BLAUSTAHL -- Page %i Line %.2i Column %.2i -- ", page, y, x);
	if (write_enabled) printf("READ-WRITE"); else printf("READ-ONLY");
	printf("  ");
	printf(VT100_CURSOR_MOVE_TO, y, x);
	fflush(stdout);
}

char editor_encode(int addr, char pc) {

	if (encryption_enabled) {
		char k = key[addr % KEY_SIZE];
		return(pc ^ k);
	} else {
		return(pc);
	}

}

char editor_decode(int addr, char cc) {

	if (encryption_enabled) {
		char k = key[addr % KEY_SIZE];
		return(cc ^ k);
	} else {
		return(cc);
	}

}

void editor_help(void) {

	mode = MODE_HELP;

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);

#ifdef CDCONLY
	printf(blaustahl_banner, BLAUSTAHL_VERSION, "CDCONLY");
#else
	printf(blaustahl_banner, BLAUSTAHL_VERSION, "COMPOSITE");
#endif

	printf(help_editor);
	printf(help_press_any_key);

}

void editor_set_password(void) {

	char password[16];

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);

	printf(help_password);

	printf("Password (0-15 characters): ");
	readline(password, 15);

	printf("\r\n");
	printf("\r\n");

	mode = MODE_HELP;

	if (!strlen(password)) {
		printf("ENCRYPTION DISABLED.\r\n");
		encryption_enabled = false;
	} else {
		printf("SETTING PASSWORD TO: '%s'\r\n", password);
	}

	printf(help_press_any_key);

	if (!strlen(password)) return;

	// password ready ...

//	encryption_enabled = true;

}

void readline(char *buf, int maxlen) {

	int c;
	int pl = 0;

	memset(buf, 0x00, maxlen + 1);

	while (1) {

		c = cdc_getchar();

		if (c == CH_CR)
			return;
		else if (c == CH_BS || c == CH_DEL) {
			pl--;
			buf[pl] = 0x00;
			printf(VT100_CURSOR_LEFT);
			printf(" ");
			printf(VT100_CURSOR_LEFT);
			printf(VT100_CURSOR_LEFT);
		}
		else if (c > 0) {
			cdc_putchar(c);
			buf[pl++] = c;
		}

		if (pl < 0) pl = 0;
		if (pl == maxlen) return;

	}

}

void editor_yield(void) {

	blaustahl_led(led);

	int redraw = 0;

	int c = cdc_getchar();
	if (c == EOF) return;

	if (mode == MODE_HELP) {
		mode = MODE_EDIT;
		printf(VT100_ERASE_SCREEN);
		printf(VT100_CLEAR_HOME);
		editor_redraw();
		return;
	}

	switch(c) {

		case(CH_ESC):
			state = STATE_ESC0;
			break;

		case(CH_FF):
			editor_redraw();
			break;

		case('['):
			if (state == STATE_ESC0)
				state = STATE_ESC1;
			break;

		default:
			if (state == STATE_ESC1) {
				state = STATE_NONE;
				if (c == 'A') { y--; printf(VT100_CURSOR_UP); }
				if (c == 'B') { y++; printf(VT100_CURSOR_DOWN); } 
				if (c == 'C') { x++; printf(VT100_CURSOR_RIGHT); }
				if (c == 'D') { x--; printf(VT100_CURSOR_LEFT); }
				if (c == 'H') { x = 1; printf(VT100_CURSOR_HOME); }
				if (c == 'F') { x = COLS; printf(VT100_CURSOR_MOVE_TO, y, x); }
				if (c == '5') { page--; redraw = 1; state = STATE_ESC2; }
				if (c == '6') { page++; redraw = 1; state = STATE_ESC2; }
				if (c == '3') {
					if (!write_enabled) break;
					int addr = ((page - 1) * PAGE_SIZE) + ((y - 1) * COLS) + (x - 1);
					blaustahl_led(LED_WRITE);
					fram_write(addr, editor_encode(addr, 0x00));
					printf(".");
					printf(VT100_CURSOR_LEFT);
					state = STATE_ESC2;
				};
				break;
			}

			if (state == STATE_ESC2) {
				state = STATE_NONE;
				break;
			}

			if (state == STATE_NONE) {
				if (c == CH_BS || c == CH_DEL) {
					if (!write_enabled) break;
					x--;
					if (!x) break;
					int addr = ((page - 1) * PAGE_SIZE) + ((y - 1) * COLS) + (x - 1);
					blaustahl_led(LED_WRITE);
					fram_write(addr, editor_encode(addr, 0x00));
					printf(VT100_CURSOR_LEFT);
					printf(".");
					printf(VT100_CURSOR_LEFT);
				} else if (c == CH_CAN) {
					if (!write_enabled) break;
					int addr = ((page - 1) * PAGE_SIZE) + ((y - 1) * COLS) + (x - 1);
					blaustahl_led(LED_WRITE);
					fram_write(addr, editor_encode(addr, 0x00));
					printf(".");
					printf(VT100_CURSOR_LEFT);
//				} else if (c == CH_DLE) {
//					editor_set_password();
				} else if (c == CH_CR) {
					printf(VT100_CURSOR_CRLF);
					x = 1;
					y += 1;
				} else if (c == CH_BEL) {
					editor_help();
				} else if (c == CH_EM) {
					blaustahl_dfu();
				} else if (c == CH_ETB) {
					if (write_enabled)
						write_enabled = false;
					else
						write_enabled = true;
				} else if (c == CH_DC1 || c == CH_DC3) {
					if (status_enabled) {
						status_enabled = false;
						redraw = true;
					} else {
						status_enabled = true;
					}
				} else if (c == CH_SOH) {
					x = 0;
				} else if (c == CH_ENQ) {
					x = COLS;
				} else {
					if (write_enabled) {
						cdc_putchar(c);
						int addr = ((page - 1) * PAGE_SIZE) +
							((y - 1) * COLS) + (x - 1);
						blaustahl_led(LED_WRITE);
						fram_write(addr, editor_encode(addr, c));
					} else if (c == '^') {
						x = 0;
					} else if (c == '$') {
						x = COLS - 1;
					}
					x++;
				}
			}
			break;

	}

	if (x < 1) x = 1;
	if (x > COLS) x = COLS;
	if (y < 1) y = 1;
	if (y > ROWS) y = ROWS;

	if (page < 1) page = 1;
	if (page > 4) page = 4;

	if (redraw)
		editor_redraw();
	else
		editor_status();

}
