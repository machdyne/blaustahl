/*
 * Grid viewer/editor for Blaustahl.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * One engine, two renderers (TEXT/ASCII, HEX), operating on whichever
 * file the FILES browser has selected (current_file, defaulting to
 * FRAM at boot -- default behavior is unchanged from the original
 * FRAM-only firmware). A single absolute byte offset (cursor_offset)
 * is the source of truth; each renderer projects it into its own
 * row/column layout, which is what lets TEXT<->HEX preserve position.
 *
 * Writing only ever happens through storage_write(), which itself only
 * ever succeeds for FRAM (storage_can_write()) -- flash is view-only.
 * That's enforced at the storage layer, not just here, so a UI mistake
 * here can't turn into a write against flash.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "blaustahl.h"
#include "editor.h"
#include "srwp.h"
#include "vt100.h"
#include "vt100_input.h"
#include "menu.h"
#include "browser.h"
#include "storage.h"
#include "cli.h"
#include "view.h"

#define ROWS 24
#define TEXT_COLS 80
#define HEX_BYTES_PER_ROW 16

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
	"CTRL-T / ESC     OPEN MENU (DOUBLE-TAP ESC FOR NO DELAY) --\r\n"
	"CTRL-F           JUMP DIRECTLY TO FILES\r\n"
	"CTRL-G           HELP (THIS SCREEN)\r\n"
	"CTRL-L           REFRESH SCREEN\r\n"
	"\r\n"
#ifdef DEV
	"CTRL-Y           FIRMWARE UPDATE MODE\r\n"
#else
	"FIRMWARE UPDATE: SEE THE CLI'S firmware_update COMMAND\r\n"
#endif
	"\r\n"
	"CTRL-C / CTRL-V  COPY / PASTE -- SHARED BY GRID EDITOR AND VIEWER\r\n"
	"\r\n"
	"GRID EDITOR (FRAM/SRAM, 7680 BYTES, 4 PAGES):\r\n"
	"  PGUP/PGDN      FLIP PAGE\r\n"
	"  CTRL-B         TOGGLE BUFFER MODE\r\n"
	"  CTRL-W         TOGGLE WRITE MODE / COMMIT BUFFER\r\n"
	"  CTRL-S/Q       TOGGLE STATUS BAR\r\n"
	"\r\n"
	"VIEWER (FLASH FILES, ANY SIZE -- READ-ONLY):\r\n"
	"  UP/DOWN        SCROLL ONE LINE\r\n"
	"  PGUP/PGDN      SCROLL ONE SCREEN\r\n"
	"  HOME           JUMP TO START\r\n"
	"\r\n"
	"FRAM SHOWS LOCKED IF ENCRYPTED -- UNLOCK: CTRL-T -> CLI -> password\r\n";

const char help_press_any_key[] =
	"\r\n"
	"Press any key to return.\r\n";

uint8_t led = LED_IDLE;

void editor(void);
void readline(char *buf, int maxlen);
void editor_help(void);

static char printable_or_dot(char c) {
	return (c > 0x1f && c < 0x7f) ? c : '.';
}

#ifdef EDITOR_MAIN
int main(int argc, char *argv[]) {

	editor_init();
	while (1) editor_yield();

	return 0;

}
#endif

int mode = MODE_GRID;

static int render_mode = 0;		// 0 = TEXT, 1 = HEX
static long cursor_offset = 0;

// shows "PRESS CTRL-G FOR HELP" in place of the edit-state field on
// the very first status line render only, then never again -- a
// first-boot hint that can't hinder normal use since it disappears
// the moment anything actually happens (any keypress redraws the
// status line through the normal path, which doesn't check this
// flag once it's been cleared).
static bool boot_hint_shown = false;

// set by editor_notify_srwp_write() (called from srwp.c after a raw
// FRAM write), shown once on the next status render, then cleared --
// same "arm once, show once" shape as the boot hint above.
static bool srwp_write_warning_pending = false;

void editor_notify_srwp_write(void) {
	srwp_write_warning_pending = true;
}

// CTRL-C toggles copy mode: first press marks the current cursor
// position as the selection's origin; cursor movement from there
// extends the highlighted range; a second CTRL-C copies that range
// (in either direction from the origin) into copy_buffer and exits.
// Confined to the current page on purpose -- PGUP/PGDN are refused
// while active rather than letting a selection span pages it can't
// actually be copied across (copy_buffer is sized for one page).
#define COPY_BUFFER_SIZE EDITOR_COPY_BUFFER_SIZE
static bool copy_mode = false;
static long copy_origin = 0;
static uint8_t copy_buffer[COPY_BUFFER_SIZE];
static uint32_t copy_buffer_len = 0;

static bool in_copy_selection(long offset) {
	if (!copy_mode) return false;
	long lo = copy_origin < cursor_offset ? copy_origin : cursor_offset;
	long hi = copy_origin < cursor_offset ? cursor_offset : copy_origin;
	return offset >= lo && offset <= hi;
}

// shared with view.c, so copying in one and pasting in the other works
// on the same underlying buffer rather than each having its own.
uint32_t editor_copy_buffer_capacity(void) {
	return COPY_BUFFER_SIZE;
}

void editor_copy_buffer_set(const uint8_t *data, uint32_t len) {
	if (len > COPY_BUFFER_SIZE) len = COPY_BUFFER_SIZE;
	memcpy(copy_buffer, data, len);
	copy_buffer_len = len;
}

uint32_t editor_copy_buffer_get(uint8_t *out, uint32_t max_len) {
	uint32_t n = copy_buffer_len < max_len ? copy_buffer_len : max_len;
	memcpy(out, copy_buffer, n);
	return n;
}

uint32_t editor_copy_buffer_len(void) {
	return copy_buffer_len;
}

bool write_enabled = false;
bool status_enabled = true;

static int bytes_per_row(void) {
	return render_mode == 0 ? TEXT_COLS : HEX_BYTES_PER_ROW;
}

static long page_size(void) {
	return (long)ROWS * bytes_per_row();
}

static long pages(void) {
	long ps = page_size();
	long n = ((long)current_file.size + ps - 1) / ps;
	return n < 1 ? 1 : n;
}

static long max_offset(void) {
	return current_file.size > 0 ? (long)current_file.size - 1 : 0;
}

// row/column-aware movement within the current page. A vertical move
// that would leave the page's valid rows clamps the row and leaves the
// column untouched -- matching the original firmware's y-clamp exactly
// (pressing DOWN on the bottom row never changed x). This is NOT the
// same as clamping the raw offset, which would snap to a different
// column -- verified against that exact bug during development.
static void move_grid(int delta_row, int delta_col) {

	long bpr = bytes_per_row();
	long ps = page_size();
	long page_start = (cursor_offset / ps) * ps;
	long page_end = page_start + ps - 1;
	if (page_end > max_offset()) page_end = max_offset();

	long offset_in_page = cursor_offset - page_start;
	long row = offset_in_page / bpr;
	long col = offset_in_page % bpr;

	long valid_in_page = page_end - page_start + 1;
	long last_row = (valid_in_page - 1) / bpr;

	row += delta_row;
	col += delta_col;

	// LEFT/RIGHT wrap to the adjacent row at the opposite edge,
	// matching conventional text-editor behavior -- without this,
	// col's clamp below would just pin you at the last/first column
	// forever, with no way to reach the next row via arrow keys alone.
	// Only wraps if that adjacent row actually exists within the
	// page; at the very first/last row it clamps instead, same as
	// before. UP/DOWN deliberately don't do this (delta_col is 0 for
	// them) -- they preserve column and clamp at page boundaries
	// only, via the row clamp just below.
	if (delta_col > 0 && col >= bpr) {
		if (row < last_row) { col = 0; row += 1; }
		else col = bpr - 1;
	}
	if (delta_col < 0 && col < 0) {
		if (row > 0) { col = bpr - 1; row -= 1; }
		else col = 0;
	}

	if (row < 0) row = 0;
	if (col < 0) col = 0;
	if (col >= bpr) col = bpr - 1;
	if (row > last_row) row = last_row;

	long new_off = page_start + row * bpr + col;
	if (new_off > page_end) new_off = page_end;

	cursor_offset = new_off;

}

// PGUP/PGDN: change page only, preserving row/column -- "no scrolling,
// only page flipping" is a deliberate, kept behavior, not an oversight.
static void change_page(int delta) {

	long ps = page_size();
	long bpr = bytes_per_row();
	long cur_page = cursor_offset / ps;
	long col_in_row = cursor_offset % bpr;
	long row_in_page = (cursor_offset % ps) / bpr;
	long total = pages();

	long new_page = cur_page + delta;
	if (new_page < 0) new_page = 0;
	if (new_page >= total) new_page = total - 1;

	long new_off = new_page * ps + row_in_page * bpr + col_in_row;
	if (new_off > max_offset()) new_off = max_offset();

	cursor_offset = new_off;

}

static void jump_row_start(void) {
	long bpr = bytes_per_row();
	cursor_offset -= cursor_offset % bpr;
}

static void jump_row_end(void) {
	long bpr = bytes_per_row();
	long row_start = cursor_offset - (cursor_offset % bpr);
	long end = row_start + bpr - 1;
	if (end > max_offset()) end = max_offset();
	cursor_offset = end;
}

static void draw_text_page(void) {

	// deliberately draws all ROWS (24) rows unconditionally, even
	// though the status line will cover row 24 when status_enabled --
	// page_size() is ROWS*bytes_per_row() (FRAM_AVAILABLE is exactly
	// 4 such pages, 80*24*4, by design), so every byte in a page must
	// actually get drawn somewhere or it becomes permanently
	// unreachable, not just visually deferred. editor_status() always
	// runs after this and overwrites row 24 with the status line when
	// enabled -- that's what makes the status line appear, not
	// skipping row 24's content here.
	//
	// Exact fixed-grid rendering: this editor is strictly for
	// FRAM/SRAM (fixed-size, exact byte-addressable) -- flash files
	// are viewed with view.c instead, which handles arbitrary sizes and
	// real line-based reflow. There is no newline-aware rendering
	// here at all.

	char buf[TEXT_COLS];
	long ps = page_size();
	long page_start = (cursor_offset / ps) * ps;

	for (int row = 0; row < ROWS; row++) {

		long row_start = page_start + (long)row * TEXT_COLS;
		uint32_t got = storage_read(current_file, row_start, buf, TEXT_COLS);

		printf(VT100_CURSOR_MOVE_TO, row + 1, 1);

		for (int col = 0; col < TEXT_COLS; col++) {
			char pc = (col < (int)got) ? buf[col] : 0x00;
			bool hl = in_copy_selection(row_start + col);
			if (hl) printf(VT100_SGR_REVERSE);
			cdc_putchar(printable_or_dot(pc));
			if (hl) printf(VT100_SGR_RESET);
		}

	}

}

static void draw_hex_page(void) {

	unsigned char buf[HEX_BYTES_PER_ROW];
	long ps = page_size();
	long page_start = (cursor_offset / ps) * ps;

	for (int row = 0; row < ROWS; row++) {

		long row_start = page_start + (long)row * HEX_BYTES_PER_ROW;
		uint32_t got = storage_read(current_file, row_start,
			(char *)buf, HEX_BYTES_PER_ROW);

		printf(VT100_CURSOR_MOVE_TO, row + 1, 1);
		printf("%06lX  ", row_start);

		for (int i = 0; i < HEX_BYTES_PER_ROW; i++) {
			bool hl = in_copy_selection(row_start + i);
			if (i < (int)got) {
				if (hl) printf(VT100_SGR_REVERSE);
				printf("%02X ", buf[i]);
				if (hl) printf(VT100_SGR_RESET);
			} else {
				printf("   ");
			}
		}

		printf(" ");

		for (int i = 0; i < HEX_BYTES_PER_ROW; i++) {
			char pc = (i < (int)got) ? (char)buf[i] : ' ';
			bool hl = in_copy_selection(row_start + i);
			if (hl) printf(VT100_SGR_REVERSE);
			cdc_putchar(printable_or_dot(pc));
			if (hl) printf(VT100_SGR_RESET);
		}

	}

}

// screen column of the first hex digit for byte `i` within a hex row
#define HEX_OFFSET_COL_WIDTH 8
static int hex_col_for_byte(int i) {
	return HEX_OFFSET_COL_WIDTH + i * 3 + 1;
}

void editor_status(void) {

	if (!status_enabled) return;

	long ps = page_size();
	int cur_page = (int)(cursor_offset / ps) + 1;
	int total_pages = (int)pages();

	char copy_state_buf[40];
	const char *edit_state;
	if (!boot_hint_shown) {
		edit_state = "PRESS CTRL-G FOR HELP";
		boot_hint_shown = true;
	}
	else if (copy_mode) {
		long lo = copy_origin < cursor_offset ? copy_origin : cursor_offset;
		long hi = copy_origin < cursor_offset ? cursor_offset : copy_origin;
		snprintf(copy_state_buf, sizeof(copy_state_buf), "COPY (%ld BYTES)", hi - lo + 1);
		edit_state = copy_state_buf;
	}
	else if (srwp_write_warning_pending) {
		edit_state = "SRWP WROTE FRAM";
		srwp_write_warning_pending = false;
	}
	else if (current_file.kind == STORAGE_FRAM &&
			storage_crypt_status() == CRYPT_LOCKED)
		edit_state = "LOCKED";
	else if (!storage_can_write(current_file)) edit_state = "VIEW";
	else if (storage_buffer_active())
		edit_state = storage_buffer_dirty() ? "BUFFER*" : "BUFFER";
	else if (write_enabled) edit_state = "EDIT";
	else edit_state = "READ-ONLY";

	printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
	printf(VT100_ERASE_LINE);
	printf("BLAUSTAHL -- %s -- %s -- PAGE %i/%i -- OFFSET %ld/%u -- %s",
		current_file.name[0] ? current_file.name : "FRAM",
		render_mode ? "HEX" : "TEXT",
		cur_page, total_pages,
		cursor_offset, current_file.size,
		edit_state);

	long page_start = (cursor_offset / ps) * ps;
	long offset_in_page = cursor_offset - page_start;
	int row = (int)(offset_in_page / bytes_per_row());
	int col = (int)(offset_in_page % bytes_per_row());

	if (render_mode == 0) {
		printf(VT100_CURSOR_MOVE_TO, row + 1, col + 1);
	} else {
		printf(VT100_CURSOR_MOVE_TO, row + 1, hex_col_for_byte(col));
	}

	fflush(stdout);

}

void editor_redraw(void) {

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);

	blaustahl_led(LED_READ);

	if (render_mode == 0) draw_text_page();
	else draw_hex_page();

	editor_status();

	fflush(stdout);

}

void editor_init(void) {

	current_file = storage_fram_ref();
	render_mode = 0;
	cursor_offset = 0;
	mode = MODE_GRID;

	editor_redraw();

}

void editor_set_render_text(void) {
	render_mode = 0;
}

void editor_set_render_hex(void) {
	render_mode = 1;
}

int editor_is_hex(void) {
	return render_mode;
}

void editor_return_to_grid(void) {
	mode = MODE_GRID;
	editor_redraw();
}

// strictly for FRAM/SRAM now -- flash files are viewed with view.c
// instead (see browser.c), so current_file.kind is always FRAM or
// SRAM whenever this is called.
void editor_open_current_file(void) {
	cursor_offset = 0;
	mode = MODE_GRID;
	editor_redraw();
}

static int mode_before_help = MODE_GRID;

void editor_help(void) {

	if (mode != MODE_HELP) mode_before_help = mode;
	mode = MODE_HELP;

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);

#ifdef CDCONLY
	printf(blaustahl_banner, BLAUSTAHL_VERSION, "CDCONLY");
#else
	printf(blaustahl_banner, BLAUSTAHL_VERSION, "COMPOSITE");
#endif

	printf("FRAM size: %i bytes\r\n", FRAM_AVAILABLE);

	printf(help_editor);
	printf(help_press_any_key);

}

int editor_mode_before_help(void) {
	return mode_before_help;
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

static void handle_key_menu(void) {
	copy_mode = false;
	view_cancel_copy();
	if (mode == MODE_MENU) menu_cancel();
	else menu_open();
}

static void handle_key_files(void) {
	if (mode == MODE_CLI) cli_cancel_pending();
	copy_mode = false;
	view_cancel_copy();
	mode = MODE_FILES;
	browser_init();
}

void editor_yield(void) {

	blaustahl_led(led);

	int redraw = 0;

	// must run before attempting to read a byte -- a lone ESC with
	// nothing following it can only ever be resolved here, since
	// there's no new byte to trigger the check otherwise
	vt100_event_t timeout_ev = vt100_input_check_timeout();
	if (timeout_ev.type == KEY_MENU) {
		handle_key_menu();
		return;
	}

	int c = cdc_getchar();
	if (c == EOF) return;

	if (c == 0) {
		srwp();
		return;
	}

	vt100_event_t ev = vt100_input_feed(c);
	if (ev.type == KEY_NONE) return;

	if (ev.type == KEY_MENU) {
		handle_key_menu();
		return;
	}

	if (ev.type == KEY_FILES) {
		handle_key_files();
		return;
	}

	if (mode == MODE_MENU)  { menu_yield(ev);    return; }
	if (mode == MODE_FILES) { browser_yield(ev); return; }
	if (mode == MODE_CLI)   { cli_yield(ev);     return; }
	if (mode == MODE_VIEW)   { view_yield(ev);   return; }

	if (mode == MODE_HELP) {
		mode = editor_mode_before_help();
		printf(VT100_ERASE_SCREEN);
		printf(VT100_CLEAR_HOME);
		if (mode == MODE_VIEW) view_redraw(); else editor_redraw();
		return;
	}

	// MODE_GRID -- TEXT or HEX render of current_file

	bool writable = storage_can_write(current_file) &&
		(storage_buffer_active() || write_enabled);

	switch (ev.type) {

		case KEY_UP:    move_grid(-1, 0); break;
		case KEY_DOWN:  move_grid(+1, 0); break;
		case KEY_LEFT:  move_grid(0, -1); break;
		case KEY_RIGHT: move_grid(0, +1); break;
		case KEY_HOME:  jump_row_start();  break;
		case KEY_END:   jump_row_end();    break;

		case KEY_PGUP:
			if (copy_mode) {
				printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
				printf(VT100_ERASE_LINE);
				printf("BLAUSTAHL -- CAN'T CROSS PAGES WHILE COPYING");
				fflush(stdout);
				break;
			}
			change_page(-1); redraw = 1; break;

		case KEY_PGDN:
			if (copy_mode) {
				printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
				printf(VT100_ERASE_LINE);
				printf("BLAUSTAHL -- CAN'T CROSS PAGES WHILE COPYING");
				fflush(stdout);
				break;
			}
			change_page(+1); redraw = 1; break;

		case KEY_COPY: {

			if (!copy_mode) {
				copy_mode = true;
				copy_origin = cursor_offset;
				redraw = 1;
				break;
			}

			long lo = copy_origin < cursor_offset ? copy_origin : cursor_offset;
			long hi = copy_origin < cursor_offset ? cursor_offset : copy_origin;
			long len = hi - lo + 1;
			if (len > COPY_BUFFER_SIZE) len = COPY_BUFFER_SIZE;

			copy_buffer_len = storage_read(current_file, lo,
				(char *)copy_buffer, (uint32_t)len);
			copy_mode = false;

			printf(VT100_CLEAR_HOME);
			printf(VT100_ERASE_SCREEN);
			if (render_mode == 0) draw_text_page(); else draw_hex_page();

			printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
			printf(VT100_ERASE_LINE);
			printf("BLAUSTAHL -- COPIED %u BYTES", copy_buffer_len);
			fflush(stdout);

			return;

		}

		case KEY_PASTE: {

			if (copy_buffer_len == 0) {
				printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
				printf(VT100_ERASE_LINE);
				printf("BLAUSTAHL -- NOTHING TO PASTE");
				fflush(stdout);
				return;
			}

			if (!writable) {
				printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
				printf(VT100_ERASE_LINE);
				printf("BLAUSTAHL -- CAN'T PASTE (NOT WRITABLE)");
				fflush(stdout);
				return;
			}

			if (cursor_offset + (long)copy_buffer_len - 1 > max_offset()) {
				printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
				printf(VT100_ERASE_LINE);
				printf("BLAUSTAHL -- PASTE WOULD EXCEED FILE BOUNDS");
				fflush(stdout);
				return;
			}

			for (uint32_t i = 0; i < copy_buffer_len; i++)
				storage_write(current_file, cursor_offset + i, (char)copy_buffer[i]);

			cursor_offset += (long)copy_buffer_len - 1;

			printf(VT100_CLEAR_HOME);
			printf(VT100_ERASE_SCREEN);
			if (render_mode == 0) draw_text_page(); else draw_hex_page();

			printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
			printf(VT100_ERASE_LINE);
			printf("BLAUSTAHL -- PASTED %u BYTES", copy_buffer_len);
			fflush(stdout);

			return;

		}

		case KEY_DEL_FWD: {
			if (!writable) break;
			storage_write(current_file, cursor_offset, 0x00);
			printf(".");
			printf(VT100_CURSOR_LEFT);
			break;
		}

		case KEY_CHAR: {

			int cc = ev.ch;

			if (cc == CH_FF) {
				editor_redraw();
				break;
			}

			if (cc == CH_BS || cc == CH_DEL) {
				if (!writable) break;
				if (cursor_offset % bytes_per_row() == 0) break;
				move_grid(0, -1);
				storage_write(current_file, cursor_offset, 0x00);
				printf(VT100_CURSOR_LEFT);
				printf(".");
				printf(VT100_CURSOR_LEFT);
			} else if (cc == CH_CAN) {
				if (!writable) break;
				storage_write(current_file, cursor_offset, 0x00);
				printf(".");
				printf(VT100_CURSOR_LEFT);
			} else if (cc == CH_CR) {
				move_grid(+1, 0);
				jump_row_start();
			} else if (cc == CH_BEL) {
				editor_help();
			} else if (cc == CH_EM) {
#ifdef DEV
				blaustahl_dfu();
#else
				// CTRL-Y is a single, easy-to-hit-by-accident
				// keystroke with no confirmation and no way back
				// (entering bootloader mode means losing anything
				// not yet committed) -- fine for active development,
				// where reflashing is routine, but too easy to trip
				// on for normal use. The CLI's firmware_update
				// command reaches the same place, but requires
				// deliberately typing a whole word first.
				printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
				printf(VT100_ERASE_LINE);
				printf("BLAUSTAHL -- USE 'firmware_update' IN THE CLI");
				fflush(stdout);
#endif
			} else if (cc == CH_STX) {
				if (storage_buffer_active()) {
					if (!storage_buffer_exit()) {
						printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
						printf(VT100_ERASE_LINE);
						printf("BLAUSTAHL -- COMMIT CHANGES FIRST (CTRL-W)");
						fflush(stdout);
						return;
					}
				} else {
					if (!storage_buffer_enter()) {
						printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
						printf(VT100_ERASE_LINE);
						printf("BLAUSTAHL -- BUFFER MODE NOT AVAILABLE HERE");
						fflush(stdout);
						return;
					}
				}
			} else if (cc == CH_ETB) {
				if (storage_buffer_active()) {
					storage_buffer_commit();
				} else {
					write_enabled = !write_enabled;
				}
			} else if (cc == CH_DC1 || cc == CH_DC3) {
				if (status_enabled) {
					status_enabled = false;
					redraw = true;
				} else {
					status_enabled = true;
				}
			} else if (cc == CH_SOH) {
				jump_row_start();
			} else if (cc == CH_ENQ) {
				jump_row_end();
			} else if (render_mode == 0) {
				// TEXT: printable chars write (if allowed) and advance
				if (writable) {
					cdc_putchar(cc);
					storage_write(current_file, cursor_offset, cc);
					move_grid(0, +1);
				} else if (cc == '^') {
					jump_row_start();
				} else if (cc == '$') {
					jump_row_end();
				} else {
					move_grid(0, +1);
				}
			} else {
				// HEX: two hex digits set one byte (high nibble, then
				// low nibble), advancing after the second digit
				static int nibble = -1;
				int v = -1;
				if (cc >= '0' && cc <= '9') v = cc - '0';
				else if (cc >= 'a' && cc <= 'f') v = cc - 'a' + 10;
				else if (cc >= 'A' && cc <= 'F') v = cc - 'A' + 10;

				if (v >= 0 && writable) {
					if (nibble < 0) {
						nibble = v;
					} else {
						storage_write(current_file, cursor_offset,
							(char)((nibble << 4) | v));
						nibble = -1;
						move_grid(0, +1);
					}
				}
			}

			break;
		}

		default:
			break;

	}

	if (redraw || copy_mode)
		editor_redraw();
	else
		editor_status();

}
