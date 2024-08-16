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
#include "ltsf.h"
#include "crypt.h"

#include "pico/stdlib.h"
#include "pico/rand.h"

#include "psa/crypto.h"

#define CRYPT_DEBUG 1

#define ROWS 24
#define COLS 80
#define PAGE_SIZE (ROWS * COLS)
#define PAGES 4

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
#define CH_STX		0x02	// CTRL-B
#define CH_ETX		0x03	// CTRL-C
#define CH_ENQ		0x05	// CTRL-E
#define CH_ACK		0x06
#define CH_BEL		0x07
#define CH_BS		0x08
#define CH_LF		0x0a
#define CH_CR		0x0d
#define CH_FF		0x0c
#define CH_DLE		0x10
#define CH_DC3		0x13
#define CH_DC4		0x14
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

#define KEY_SIZE 32

const char blaustahl_banner[] =
	"BLAUSTAHL FIRMWARE V%s %s\r\n"
	"Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.\r\n"
	"\r\n";

const char help_encryption[] =
	"\r\n"
	"WARNING: Encryption is an experimental feature, please backup\r\n"
	"your data before using encryption.\r\n"
	"\r\n"
	"If in buffer mode, make sure the buffer is saved before continuing.\r\n"
	"\r\n"
	"In order to enable encryption, enter a password/passphrase below,\r\n"
	"or press ENTER to abort.\r\n"
	"\r\n";

const char help_password[] =
	"\r\n"
	"WARNING: Encryption is an experimental feature, please backup\r\n"
	"your data before using encryption.\r\n"
	"\r\n"
	"Encrypted data has been detected, enter your password/passphrase below.\r\n"
	"\r\n";

const char help_editor[] =
	"COMMANDS:\r\n"
	"\r\n"
	"CTRL-G    HELP\r\n"
	"CTRL-L    REFRESH SCREEN\r\n"
	"CTRL-B    TOGGLE BUFFER MODE\r\n"
	"CTRL-W    TOGGLE WRITE MODE or WRITE BUFFER\r\n"
	"CTRL-S    TOGGLE STATUS BAR\r\n"
	"CTRL-P    SET ENCRYPTION PASSWORD\r\n"
	"CTRL-C    COMMAND LINE INTERFACE\r\n"
	"CTRL-Y    ENTER FIRMWARE UPDATE MODE\r\n";

const char help_cli[] =
	"COMMANDS:\r\n"
	"\r\n"
	"disable_encryption  disable encryption and save buffer as plaintext\r\n"
	"nuke                disable encryption and erase all memory\r\n"
	"exit                return to editor\r\n";

const char help_press_any_key[] =
	"\r\n"
	"Press SPACE to return to the editor.\r\n";

uint8_t led = LED_IDLE;

void editor(void);
void editor_set_password(void);
void readline(char *buf, int maxlen);
uint32_t xorshift32(uint32_t state[]);
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
bool mode_crypt = false;
bool crypt_valid = false;
bool mode_buffer = false;
bool status_enabled = true;
uint8_t key[KEY_SIZE];
psa_key_id_t key_id;

uint8_t ebuf[COLS*ROWS*PAGES];
ltsf_meta_t meta;

void editor_init(void);
void editor_load_meta(void);
void editor_save_meta(void);
void editor_load(void);
void editor_save(void);
void editor_status(void);
void editor_status_set_msg(char *msg);
void editor_disable_encryption(void);
void editor_nuke(void);

char editor_status_msg[16];

void editor_init(void) {

	editor_load_meta();

	// increment and update boot counter
	meta.bootctr += 1;
	fram_write((COLS*ROWS*PAGES)+128-4, (char *)&meta.bootctr, 4);

	if (meta.algo != LTSF_ALGO_PLAINTEXT) {
		mode_crypt = true;
		mode_buffer = true;
	}

	cdc_print(VT100_CLEAR_HOME);
	cdc_print(VT100_ERASE_SCREEN);

	page = 1;
	x = 1;
	y = 1;

	editor_redraw();

}

void editor_load_meta(void) {

	uint8_t mbuf[128];
	fram_read(mbuf, COLS*ROWS*PAGES, 128);
	memcpy(&meta.magic, &mbuf[0], 2);
	memcpy(&meta.version, &mbuf[2], 1);
	memcpy(&meta.algo, &mbuf[3], 1);
	bzero(&meta.plaindesc, 48);
	memcpy(&meta.plaindesc, &mbuf[4], 47);
	memcpy(&meta.salt, &mbuf[52], 16);
	memcpy(&meta.nonce, &mbuf[68], 12);
	memcpy(&meta.tag, &mbuf[92], 16);
	memcpy(&meta.bootctr, &mbuf[124], 4);

}

void editor_save_meta(void) {

	uint8_t mbuf[128];
	memcpy(&mbuf[0], &meta.magic, 2);
	memcpy(&mbuf[2], &meta.version, 1);
	memcpy(&mbuf[3], &meta.algo, 1);
	memcpy(&mbuf[4], &meta.plaindesc, 48);
	memcpy(&mbuf[52], &meta.salt, 16);
	memcpy(&mbuf[68], &meta.nonce, 12);
	memcpy(&mbuf[92], &meta.tag, 16);
	memcpy(&mbuf[124], &meta.bootctr, 4);
	fram_write(COLS*ROWS*PAGES, mbuf, 128);

}

void editor_load(void) {

	blaustahl_led(LED_READ);

	if (mode_crypt) {

		uint8_t ctbuf[COLS*ROWS*PAGES+16];
		size_t pt_len;
		uint8_t pos[4] = { 0x00, 0x00, 0x00, 0x01 };
		fram_read(ctbuf, 0, COLS*ROWS*PAGES);
		memcpy(&ctbuf[COLS*ROWS*PAGES], &meta.tag, 16);
   	int ok = crypt_decrypt(key_id, meta.nonce, pos, ctbuf, COLS*ROWS*PAGES+16,
			ebuf, COLS*ROWS*PAGES, &pt_len);

		//if (ok && pt_len == COLS*ROWS*PAGES)
		if (!ok)
			cdc_print("DECRYPTION FAILED.\r\n");
		else
			crypt_valid = true;

	} else {
		fram_read(ebuf, 0, COLS*ROWS*PAGES);
	}

}

void editor_save(void) {

	blaustahl_led(LED_WRITE);

	if (mode_crypt) {

		if (!crypt_valid) {
			cdc_print("Enter correct password first.");
			return;
		}

		uint8_t ctbuf[COLS*ROWS*PAGES+16];
		size_t ct_len;
		uint8_t pos[4] = { 0x00, 0x00, 0x00, 0x01 };
		crypt_nonce_inc((uint8_t *)&meta.nonce);
   	int ok = crypt_encrypt(key_id, meta.nonce, pos, ebuf, COLS*ROWS*PAGES,
			ctbuf, COLS*ROWS*PAGES+16, &ct_len);

		if (!ok) {
			cdc_print("ENCRYPTION FAILED.\r\n");
			return;
		}

		fram_write(0, ctbuf, COLS*ROWS*PAGES);
		memcpy(&meta.tag[0], &ctbuf[COLS*ROWS*PAGES], 16);
		editor_save_meta();

	} else {
		fram_write(0, ebuf, COLS*ROWS*PAGES);
	}

	editor_status_set_msg("SAVED");

}

void editor_redraw(void) {

	char buf[COLS];
	editor_status();
	cdc_printf(VT100_CURSOR_MOVE_TO, 24, 59);
	cdc_print("PRESS CTRL-G FOR HELP");

	blaustahl_led(LED_READ);

	if (mode_buffer) {
		for (int y = 0; y < ROWS; y++) {
			cdc_printf(VT100_CURSOR_MOVE_TO, y + 1, 1);
			for (int x = 0; x < COLS; x++) {
				char pc = ebuf[((page - 1) * ROWS * COLS) + (y * COLS) + x];
				if (pc > 0x1f && pc < 0x7f)
					cdc_putchar(pc);
				else
					cdc_putchar('.');
			}
		}
	} else {
		for (int y = 0; y < ROWS; y++) {
			if (status_enabled && y == ROWS - 1) continue;
			fram_read(buf, ((page - 1) * PAGE_SIZE) + (y * COLS), COLS);
			cdc_printf(VT100_CURSOR_MOVE_TO, y + 1, 1);
			for (int x = 0; x < COLS; x++) {
				char pc = buf[x];
				if (pc > 0x1f && pc < 0x7f)
						cdc_putchar(pc);
				else
					cdc_putchar('.');
			}
		}
	}

	cdc_printf(VT100_CURSOR_MOVE_TO, y, x);
	fflush(stdout);
}

void editor_status_set_msg(char *msg) {
	strncpy(editor_status_msg, msg, 15);
}

void editor_status(void) {
	if (!status_enabled) return;
	cdc_printf(VT100_CURSOR_MOVE_TO, 24, 1);
	cdc_printf("BLAUSTAHL -- Page %i Line %.2i Column %.2i -- ", page, y, x);

	if (editor_status_msg[0] != 0x00) {

		cdc_print(editor_status_msg);
		editor_status_msg[0] = 0x00;

	} else {

		if (mode_buffer) {
			cdc_print("BUFFER");
		} else {
			if (write_enabled)
				cdc_print("READ-WRITE");
			else
				cdc_print("READ-ONLY");
		}

	}
	cdc_print("  ");
	cdc_printf(VT100_CURSOR_MOVE_TO, y, x);
	fflush(stdout);
}

void editor_help(void) {

	mode = MODE_HELP;

	cdc_print(VT100_CLEAR_HOME);
	cdc_print(VT100_ERASE_SCREEN);

#ifdef CDCONLY
	cdc_printf(blaustahl_banner, BLAUSTAHL_VERSION, "CDCONLY");
#else
	cdc_printf(blaustahl_banner, BLAUSTAHL_VERSION, "COMPOSITE");
#endif

	editor_load_meta();

	cdc_printf("MAGIC  : %.4x ", meta.magic);
	cdc_printf("VERSION: %.2x ", meta.version);
	cdc_printf("ALGO: %.2x\r\n", meta.algo);
	cdc_printf("DESC   : %s\r\n", meta.plaindesc);

#ifdef CRYPT_DEBUG
	cdc_printf("SALT   : ");
	for(int i = 0; i < 16; i++) {
		cdc_printf("%02x", meta.salt[i]);
	}
	cdc_printf("\r\n");

	cdc_printf("NONCE  : ");
	for(int i = 0; i < 12; i++) {
		cdc_printf("%02x", meta.nonce[i]);
	}
	cdc_printf("\r\n");

	cdc_printf("TAG    : ");
	for(int i = 0; i < 16; i++) {
		cdc_printf("%02x", meta.tag[i]);
	}
	cdc_printf("\r\n");
#endif

	cdc_printf("BOOTCTR: %.8x\r\n", meta.bootctr);

	cdc_printf("\r\n");

	cdc_print(help_editor);
	cdc_print(help_press_any_key);

}

void editor_cli(void) {

	char cmd[33];

	cdc_print(VT100_CLEAR_HOME);
	cdc_print(VT100_ERASE_SCREEN);

	cdc_print(help_cli);

	while (1) {

		cdc_print("Blaustahl> ");

		readline(cmd, 32);
		cdc_print("\r\n");

		if (!strncmp(cmd, "disable_encryption", 32)) {
			editor_disable_encryption();
		}
		if (!strncmp(cmd, "nuke", 32)) {
			editor_nuke();
		}
		else if (!strncmp(cmd, "exit", 32)) {
			mode = MODE_HELP;
			cdc_print(help_press_any_key);
			break;
		}

	}

}

void editor_nuke(void) {

	char nuke_confirm[32];

	cdc_print("To erase everything type: ERASE EVERYTHING NOW\r\n");
	cdc_print("or press ENTER to abort.\r\n");

	readline(nuke_confirm, 32);
	cdc_print("\r\n");

	if (!strncmp(nuke_confirm, "ERASE EVERYTHING NOW", 32)) {

		for (int addr = 0; addr < (COLS*ROWS*PAGES)+128; addr++)
			fram_write_byte(addr, 0x00);

		// reinitialize metadata
		editor_load_meta();

		meta.magic = LTSF_MAGIC;
		meta.version = 0x00;
		meta.algo = 0x00;
		strncpy(meta.plaindesc, "plaintext", 48);

		editor_save_meta();

		mode_crypt = false;
		crypt_valid = false;

		cdc_print("Device has been nuked.\r\n");

	}

}

void editor_set_password(void) {

	char password[33];
	char password_confirm[33];

	cdc_print(VT100_CLEAR_HOME);
	cdc_print(VT100_ERASE_SCREEN);

	if (mode_crypt)
		cdc_print(help_password);
	else
		cdc_print(help_encryption);

	cdc_print("Password (1-32 characters): ");
	readline(password, 32);

	if (!strlen(password)) {
		mode = MODE_HELP;
		cdc_print(help_press_any_key);
		return;
	}

	cdc_print("\r\n");
	cdc_print("\r\n");

	crypt_valid = false;

	if (meta.algo != LTSF_ALGO_PLAINTEXT) {

		// concat password and salt
		uint8_t pass_salt[32+16];
		memcpy(&pass_salt[0], password, 32);
		memcpy(&pass_salt[32], &meta.salt, 16);

		// KDF with saved salt
		size_t key_len;
		psa_status_t status;
		status = psa_hash_compute(PSA_ALG_SHA_256,
			pass_salt, 32+16,
			(unsigned char *)&key, 32,
			&key_len);

		if (status != PSA_SUCCESS) {
			cdc_printf("KDF FAILED.");
			return;
		} else {
#ifdef CRYPT_DEBUG
    		cdc_printf("KEY: ");
    		for(int i = 0; i < 32; i++) {
   		     cdc_printf("%02x", key[i]);
   		 }
   		 cdc_printf("\r\n");
#endif
		}

		int ok = crypt_init(&key_id, key);
		if (ok) {
			cdc_print("KEY INITIALIZATION OK.");
		} else {
			cdc_print("KEY INITIALIZATION FAILED.");
			return;
		}

		cdc_printf("\r\n");

		mode_buffer = true;
		mode_crypt = true;

		editor_load();

		mode = MODE_HELP;
		cdc_print(help_press_any_key);

		return;

	}

	cdc_print("Confirm password: ");
	readline(password_confirm, 32);

	cdc_print("\r\n\r\n");

	if (strncmp(password, password_confirm, 32)) {
		cdc_print("PASSWORD MISMATCH.\r\n");
		mode = MODE_HELP;
		cdc_print(help_press_any_key);
		return;
	}

	// generate salt
	uint64_t r;
	r = get_rand_32();
	memcpy(&meta.salt[0], &r, 4);
	r = get_rand_32();
	memcpy(&meta.salt[4], &r, 4);
	r = get_rand_32();
	memcpy(&meta.salt[8], &r, 4);
	r = get_rand_32();
	memcpy(&meta.salt[12], &r, 4);

	// reset nonce
	bzero(&meta.nonce[0], 12);

	// concat password and salt
	uint8_t pass_salt[32+16];
	memcpy(&pass_salt[0], password, 32);
	memcpy(&pass_salt[32], &meta.salt, 16);

	// KDF
	size_t key_len;
	psa_status_t status;
	status = psa_hash_compute(PSA_ALG_SHA_256,
		pass_salt, 32+16,
		(unsigned char *)&key, 32,
		&key_len);

	cdc_printf("SALT: ");
	for(int i = 0; i < 16; i++) {
		cdc_printf("%02x", meta.salt[i]);
	}
	cdc_printf("\r\n");

	cdc_printf("KEY: ");
	for(int i = 0; i < 32; i++) {
		cdc_printf("%02x", key[i]);
	}
	cdc_printf("\r\n");

	int ok = crypt_init(&key_id, key);
	if (ok)
		cdc_print("KEY INITIALIZATION OK.");
	else
		cdc_print("KEY INITIALIZATION FAILED.");

	fram_read(ebuf, 0, COLS*ROWS*PAGES);

	mode_buffer = true;
	mode_crypt = true;
	crypt_valid = true;

	// initialize metadata
	meta.magic = LTSF_MAGIC;
	meta.version = 0x00;
	meta.algo = 0x01;
	strncpy(meta.plaindesc, "SHA256(p||salt)+ChaCha20-Poly1305", 48);

	// write encrypted text and metadata to FRAM
	editor_save();

	mode = MODE_HELP;
	cdc_print(help_press_any_key);

}

void editor_disable_encryption(void) {

	if (!crypt_valid) {
		cdc_printf("Enter valid password first.\r\n");
		return;
	}

	// initialize metadata
	meta.magic = LTSF_MAGIC;
	meta.version = 0x00;
	meta.algo = 0x00;
	strncpy(meta.plaindesc, "plaintext", 48);

	editor_save_meta();

	mode_crypt = false;

	editor_save();

	cdc_print("Encryption disabled.\r\n");

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
			cdc_print(VT100_CURSOR_LEFT);
			cdc_print(" ");
			cdc_print(VT100_CURSOR_LEFT);
			cdc_print(VT100_CURSOR_LEFT);
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
	if (c <= 0) return;

	if (mode == MODE_HELP) {
		mode = MODE_EDIT;
		cdc_print(VT100_ERASE_SCREEN);
		cdc_print(VT100_CLEAR_HOME);
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
				if (c == 'A') { y--; cdc_print(VT100_CURSOR_UP); }
				if (c == 'B') { y++; cdc_print(VT100_CURSOR_DOWN); } 
				if (c == 'C') { x++; cdc_print(VT100_CURSOR_RIGHT); }
				if (c == 'D') { x--; cdc_print(VT100_CURSOR_LEFT); }
				if (c == 'H') { x = 1; cdc_print(VT100_CURSOR_HOME); }
				if (c == 'F') { x = COLS; cdc_printf(VT100_CURSOR_MOVE_TO, y, x); }
				if (c == '5') { page--; redraw = 1; state = STATE_ESC2; }
				if (c == '6') { page++; redraw = 1; state = STATE_ESC2; }
				if (c == '3') {
					if (!mode_buffer && !write_enabled) break;
					int addr = ((page - 1) * PAGE_SIZE) + ((y - 1) * COLS) + (x - 1);
					if (mode_buffer) {
						ebuf[addr] = 0x00;
					} else {
						blaustahl_led(LED_WRITE);
						fram_write_byte(addr, 0x00);
					}
					cdc_print(".");
					cdc_print(VT100_CURSOR_LEFT);
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
					if (!mode_buffer && !write_enabled) break;
					x--;
					if (!x) break;
					int addr = ((page - 1) * PAGE_SIZE) + ((y - 1) * COLS) + (x - 1);
					if (mode_buffer) {
						ebuf[addr] = 0x00;
					} else {
						blaustahl_led(LED_WRITE);
						fram_write_byte(addr, 0x00);
					}
					cdc_print(VT100_CURSOR_LEFT);
					cdc_print(".");
					cdc_print(VT100_CURSOR_LEFT);
				} else if (c == CH_CAN) {
					if (!mode_buffer && !write_enabled) break;
					int addr = ((page - 1) * PAGE_SIZE) + ((y - 1) * COLS) + (x - 1);
					if (mode_buffer) {
						ebuf[addr] = 0x00;
					} else {
						blaustahl_led(LED_WRITE);
						fram_write_byte(addr, 0x00);
					}
					cdc_print(".");
					cdc_print(VT100_CURSOR_LEFT);
				} else if (c == CH_DLE) {
					editor_set_password();
				} else if (c == CH_CR) {
					cdc_print(VT100_CURSOR_CRLF);
					x = 1;
					y += 1;
				} else if (c == CH_BEL) {
					editor_help();
				} else if (c == CH_ETX) {
					editor_cli();
				} else if (c == CH_EM) {
					blaustahl_dfu();
				} else if (c == CH_ETB) {
					if (mode_buffer) {
						editor_save();
					} else {
						if (write_enabled)
							write_enabled = false;
						else
							write_enabled = true;
					}
				} else if (c == CH_DC3) {
					if (status_enabled) {
						status_enabled = false;
						redraw = true;
					} else {
						status_enabled = true;
					}
				} else if (c == CH_STX) {
					if (mode_buffer) {
						if (!mode_crypt) mode_buffer = false;
					} else {
						mode_buffer = true;
						editor_load();
					}
					redraw = true;
				} else if (c == CH_SOH) {
					x = 0;
				} else if (c == CH_ENQ) {
					x = COLS;
				} else {
					if (mode_buffer || write_enabled) {
						cdc_putchar(c);
						int addr = ((page - 1) * PAGE_SIZE) +
							((y - 1) * COLS) + (x - 1);
						if (mode_buffer) {
							ebuf[addr] = c;
						} else {
							blaustahl_led(LED_WRITE);
							fram_write_byte(addr, c);
						}
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
