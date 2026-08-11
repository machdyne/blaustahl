/*
 * Mode menu bar for Blaustahl.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * A single-line VT100 command bar, three visual groups on one row:
 *
 *   /// | EDITOR: [FRAM] [SRAM] | [VIEWER] [FILES] [CLI] [HELP] |   MODE: [TEXT] [HEX]
 *   (logo)  (grid editor's         (navigation, closes menu)      (render pref)
 *            fixed target)
 *
 * FRAM and SRAM are dedicated items rather than entries in the file
 * browser -- they're always exactly two fixed, known targets for the
 * grid editor, not something that needs browsing. Selecting whichever
 * one the editor ISN'T currently on switches to it (resetting cursor
 * position, since that's a genuinely different file); selecting the
 * one it's already on just returns to the grid, preserving position.
 *
 * VIEWER/FILES/CLI/HELP are NAVIGATION: selecting one switches mode
 * and closes the menu. MODE (TEXT/HEX) is a SETTING: selecting it only
 * changes the render preference for the grid editor, and does NOT
 * close the menu. One flat left/right cursor moves across all items in
 * visual order; Enter's effect depends on which item it's on.
 *
 * VIEWER returns to whatever view.c is currently showing, or falls
 * through to FILES if nothing's ever been opened in the viewer yet --
 * there's always somewhere useful to land rather than an empty screen.
 *
 * Two independent visual markers, since "where the cursor currently
 * is" and "what's actually active right now" are not always the same
 * item: REVERSE VIDEO = cursor position, UNDERLINE = currently active
 * (FRAM or SRAM for the editor pair, whichever app for the nav group,
 * render preference for MODE) -- both at once when they coincide,
 * which is the common case.
 *
 * CTRL-T is handled once, centrally, by editor.c's dispatcher via
 * vt100_input.c -- menu.c only ever sees KEY_LEFT/KEY_RIGHT/KEY_CHAR(CR),
 * never has to detect its own close. That dispatcher checks for
 * KEY_MENU before the MODE_HELP branch, so opening the menu from the
 * help screen is a real, reachable path -- HELP is treated as a fully
 * normal item, including being preselected/shown active there.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "editor.h"
#include "browser.h"
#include "storage.h"
#include "cli.h"
#include "view.h"
#include "vt100.h"
#include "vt100_input.h"
#include "menu.h"

enum {
	ITEM_FRAM = 0, ITEM_SRAM,
	ITEM_VIEWER, ITEM_FILES, ITEM_CLI, ITEM_HELP,
	ITEM_TEXT, ITEM_HEX,
	ITEM_COUNT
};

#define RIGHT_BLOCK_COL 63	// 80 - strlen("MODE: [TEXT] [HEX]") + 1

static int selected = 0;
static int mode_before_menu = MODE_GRID;

static bool app_is_active(int item) {
	switch (item) {
		case ITEM_FRAM:   return mode_before_menu == MODE_GRID && current_file.kind == STORAGE_FRAM;
		case ITEM_SRAM:   return mode_before_menu == MODE_GRID && current_file.kind == STORAGE_SRAM;
		case ITEM_VIEWER: return mode_before_menu == MODE_VIEW;
		case ITEM_FILES:  return mode_before_menu == MODE_FILES;
		case ITEM_CLI:    return mode_before_menu == MODE_CLI;
		case ITEM_HELP:   return mode_before_menu == MODE_HELP;
	}
	return false;
}

static void menu_draw(void) {

	printf(VT100_CURSOR_MOVE_TO, 1, 1);
	printf(VT100_ERASE_LINE);

	printf("/// | EDITOR: ");

	static const char *editor_labels[2] = { "FRAM", "SRAM" };
	for (int i = 0; i < 2; i++) {

		int idx = ITEM_FRAM + i;
		bool is_cursor = (selected == idx);
		bool is_active = app_is_active(idx);

		if (is_cursor) printf(VT100_SGR_REVERSE);
		if (is_active) printf(VT100_SGR_UNDERLINE);
		printf("[%s]", editor_labels[i]);
		if (is_cursor || is_active) printf(VT100_SGR_RESET);

		putchar(' ');

	}

	printf("| ");

	static const char *nav_labels[4] = { "VIEWER", "FILES", "CLI", "HELP" };
	for (int i = 0; i < 4; i++) {

		int idx = ITEM_VIEWER + i;
		bool is_cursor = (selected == idx);
		bool is_active = app_is_active(idx);

		if (is_cursor) printf(VT100_SGR_REVERSE);
		if (is_active) printf(VT100_SGR_UNDERLINE);
		printf("[%s]", nav_labels[i]);
		if (is_cursor || is_active) printf(VT100_SGR_RESET);

		putchar(' ');

	}

	printf("|");

	// right group: MODE (TEXT/HEX render preference), right-justified
	// to the screen edge
	printf(VT100_CURSOR_MOVE_TO, 1, RIGHT_BLOCK_COL);
	printf("MODE: ");

	static const char *mode_labels[2] = { "TEXT", "HEX" };
	for (int i = 0; i < 2; i++) {

		int idx = ITEM_TEXT + i;
		bool is_cursor = (selected == idx);
		bool is_active = (i == editor_is_hex());

		if (is_cursor) printf(VT100_SGR_REVERSE);
		if (is_active) printf(VT100_SGR_UNDERLINE);
		printf("[%s]", mode_labels[i]);
		if (is_cursor || is_active) printf(VT100_SGR_RESET);

		if (i == 0) putchar(' ');

	}

	fflush(stdout);

}

void menu_open(void) {

	if (mode == MODE_MENU) return;		// already open -- ignore, don't
										// let mode_before_menu get clobbered

	// leaving CLI mid-confirmation (e.g. "format" awaiting YES) defuses
	// the pending destructive action rather than leaving it silently
	// armed for whenever CLI is next visited
	if (mode == MODE_CLI) cli_cancel_pending();

	// same idea for an in-progress copy selection in view.c
	if (mode == MODE_VIEW) view_cancel_copy();

	mode_before_menu = mode;
	mode = MODE_MENU;

	switch (mode_before_menu) {
		case MODE_VIEW:  selected = ITEM_VIEWER; break;
		case MODE_FILES: selected = ITEM_FILES;  break;
		case MODE_CLI:   selected = ITEM_CLI;    break;
		case MODE_HELP:  selected = ITEM_HELP;   break;
		case MODE_GRID:
			selected = (current_file.kind == STORAGE_SRAM) ? ITEM_SRAM : ITEM_FRAM;
			break;
		default:
			selected = ITEM_FRAM;
			break;
	}

	menu_draw();

}

void menu_cancel(void) {

	mode = mode_before_menu;

	switch (mode) {
		case MODE_VIEW:  view_redraw();    break;
		case MODE_FILES: browser_redraw(); break;
		case MODE_CLI:   cli_redraw();     break;
		case MODE_HELP:  editor_help();    break;
		case MODE_GRID:
		default:         editor_redraw();  break;
	}

}

static void menu_move(int delta) {

	selected += delta;
	if (selected < 0) selected = ITEM_COUNT - 1;
	if (selected >= ITEM_COUNT) selected = 0;

	menu_draw();

}

// switches the grid editor to `f` (FRAM or SRAM): just returns to the
// grid, preserving cursor position, if it's already the active file;
// otherwise switches to it, resetting position, since that's a
// genuinely different file. FRAM and SRAM each keep their own
// independent buffer (see storage.c), so switching never discards
// unsaved edits on either side -- storage_select() can't actually be
// refused anymore, but the check below is kept as a defensive no-op
// in case a future failure mode is ever added there.
static void select_editor_file(file_ref_t f) {

	if (current_file.kind == f.kind) {
		editor_return_to_grid();
		return;
	}

	if (!storage_select(f)) {
		mode = MODE_GRID;
		printf(VT100_CLEAR_HOME);
		printf(VT100_ERASE_SCREEN);
		printf(VT100_CURSOR_MOVE_TO, 24, 1);
		printf("BLAUSTAHL -- COMMIT (CTRL-W) OR EXIT (CTRL-B) "
			"THE BUFFER BEFORE SWITCHING FILES");
		fflush(stdout);
		return;
	}

	editor_open_current_file();

}

static void menu_select(void) {

	switch (selected) {

		case ITEM_FRAM:
			select_editor_file(storage_fram_ref());
			break;

		case ITEM_SRAM:
			select_editor_file(storage_sram_ref());
			break;

		case ITEM_VIEWER:
			// nothing's ever been opened in the viewer yet -- FILES
			// is the useful place to land, not an empty screen
			if (view_has_file()) view_return();
			else { mode = MODE_FILES; browser_init(); }
			break;

		case ITEM_FILES:
			mode = MODE_FILES;
			browser_init();
			break;

		case ITEM_CLI:
			mode = MODE_CLI;
			cli_init();
			break;

		case ITEM_HELP:
			mode = mode_before_menu;
			editor_help();
			break;

		case ITEM_TEXT:
			editor_set_render_text();
			menu_draw();	// refresh so the MODE group's underline moves
			break;

		case ITEM_HEX:
			editor_set_render_hex();
			menu_draw();
			break;

	}

}

void menu_yield(vt100_event_t ev) {

	if (ev.type == KEY_LEFT)  { menu_move(-1); return; }
	if (ev.type == KEY_RIGHT) { menu_move(1);  return; }

	if (ev.type == KEY_CHAR && ev.ch == CH_CR) {
		menu_select();
		return;
	}

}
