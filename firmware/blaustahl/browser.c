/*
 * File browser for Blaustahl.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Flash files only -- FRAM and SRAM are no longer listed here at all.
 * They're reached directly via the menu's dedicated [FRAM]/[SRAM]
 * items instead, since they're always exactly two fixed, known
 * entries; browsing never made much sense for them the way it does
 * for an arbitrary, growing list of flash files.
 *
 * Enter opens the selected file in view.c -- it tracks its own file
 * reference independent of the grid editor's current_file, so
 * browsing/viewing flash files never disturbs whatever FRAM/SRAM
 * state the editor is in.
 *
 * Reopening the browser preselects whatever view.c is currently
 * showing, if anything -- there's no other "current file" concept
 * left to preselect now that FRAM/SRAM aren't part of this list.
 *
 * Scrolls once the entry count exceeds BROWSER_LIST_ROWS: `selected`
 * is a LOGICAL index into the full list, `scroll_top` is the logical
 * index of whichever entry is at the top of the visible window.
 * ensure_selected_visible() is the single place that keeps these two
 * consistent -- used both when moving the cursor and when the browser
 * first opens (so jumping straight to a file far down a long list, via
 * "current file" preselection, correctly scrolls it into view rather
 * than leaving the highlight off-screen). Moving within the current
 * window only redraws the two affected rows; scrolling redraws the
 * whole visible page, since every row's content shifts.
 */

#include <stdio.h>
#include <string.h>

#include "vt100.h"
#include "vt100_input.h"
#include "storage.h"
#include "editor.h"
#include "view.h"
#include "browser.h"

#define BROWSER_COLS 80
#define BROWSER_LIST_ROWS 23	// row 24 is reserved for the status line

static int selected = 0;		// logical index into the flash file list
static int scroll_top = 0;		// logical index of the topmost visible row

// caches the flash-file entries (kind/name/size) for whatever window
// is currently visible, populated in one batch scan (see
// refresh_flash_cache()) rather than one directory scan per row --
// storage_file_at() re-scans the whole directory from the start on
// every call, which is measurably slow on real flash once there's
// more than a handful of files and every visible row calls it on
// every redraw.
static file_ref_t flash_cache[BROWSER_LIST_ROWS];
static int flash_cache_start = -1;
static int flash_cache_count = 0;

static int browser_entry_count(void) {
	return storage_file_count();
}

static void refresh_flash_cache(void) {

	int count = BROWSER_LIST_ROWS;

	flash_cache_count = storage_flash_file_range(scroll_top, count, flash_cache);
	flash_cache_start = scroll_top;

}

static file_ref_t browser_entry(int idx) {

	if (flash_cache_start >= 0 && idx >= flash_cache_start &&
			idx < flash_cache_start + flash_cache_count) {
		return flash_cache[idx - flash_cache_start];
	}

	// not cached (shouldn't normally happen -- the cache is always
	// refreshed to cover the current window -- but stay correct if it
	// somehow isn't) -- falls back to a direct, slower single lookup
	return storage_file_at(idx);

}

// keeps scroll_top consistent with selected and the current entry
// count -- the one place both browser_move() and browser_init() defer
// to, so "jump to a file far down the list" and "arrow past the edge
// of the visible window" scroll correctly by the same logic
static void ensure_selected_visible(void) {

	int n = browser_entry_count();

	if (selected < 0) selected = 0;
	if (selected >= n) selected = n > 0 ? n - 1 : 0;

	if (selected < scroll_top) scroll_top = selected;
	if (selected >= scroll_top + BROWSER_LIST_ROWS)
		scroll_top = selected - BROWSER_LIST_ROWS + 1;

	int max_scroll = n - BROWSER_LIST_ROWS;
	if (max_scroll < 0) max_scroll = 0;
	if (scroll_top > max_scroll) scroll_top = max_scroll;
	if (scroll_top < 0) scroll_top = 0;

}

static void browser_draw_row(int screen_row) {

	if (screen_row < 0 || screen_row >= BROWSER_LIST_ROWS) return;

	int idx = scroll_top + screen_row;
	int n = browser_entry_count();

	printf(VT100_CURSOR_MOVE_TO, screen_row + 1, 1);
	printf(VT100_ERASE_LINE);

	if (idx >= n) {
		fflush(stdout);
		return;		// past the end of the list -- leave the row blank
	}

	file_ref_t f = browser_entry(idx);

	if (idx == selected) printf(VT100_SGR_REVERSE);

	char line[BROWSER_COLS + 1];
	snprintf(line, sizeof(line), "%-40s %8u BYTES", f.name, f.size);

	printf("%-*s", BROWSER_COLS, line);

	if (idx == selected) printf(VT100_SGR_RESET);

	fflush(stdout);

}

static void browser_status(void) {

	uint32_t free_kb = storage_flash_free() / 1024;
	uint32_t total_kb = storage_flash_total() / 1024;
	int n = browser_entry_count();

	printf(VT100_CURSOR_MOVE_TO, 24, 1);
	printf(VT100_ERASE_LINE);

	const char *viewing = view_has_file() ? view_current_file().name : "(NONE)";

	if (n > BROWSER_LIST_ROWS) {

		int first_shown = scroll_top + 1;
		int last_shown = scroll_top + BROWSER_LIST_ROWS;
		if (last_shown > n) last_shown = n;

		printf("BLAUSTAHL -- %i FILES (%i-%i/%i) -- %u/%u KB FREE -- VIEW: %s",
			n, first_shown, last_shown, n,
			free_kb, total_kb, viewing);

	} else {

		printf("BLAUSTAHL -- %i FILES -- %u/%u KB FREE -- VIEW: %s",
			n, free_kb, total_kb, viewing);

	}

	fflush(stdout);

}

void browser_redraw(void) {

	refresh_flash_cache();

	printf(VT100_ERASE_SCREEN);
	printf(VT100_CLEAR_HOME);

	for (int i = 0; i < BROWSER_LIST_ROWS; i++)
		browser_draw_row(i);

	browser_status();

}

static int find_current_index(void) {

	if (!view_has_file()) return 0;

	file_ref_t target = view_current_file();

	int n = storage_file_count();
	for (int i = 0; i < n; i++) {
		file_ref_t f = storage_file_at(i);
		if (strcmp(f.name, target.name) == 0)
			return i;
	}

	return 0;	// not found (e.g. it was deleted) -- fall back to the top

}

void browser_init(void) {
	selected = find_current_index();
	ensure_selected_visible();
	browser_redraw();
}

static void browser_move(int delta) {

	int old_selected = selected;
	int old_scroll_top = scroll_top;

	selected += delta;
	ensure_selected_visible();

	if (scroll_top != old_scroll_top) {
		// the whole visible window shifted -- every row's content
		// changed, not just which one is highlighted, and the cache
		// needs to cover the new window
		refresh_flash_cache();
		for (int i = 0; i < BROWSER_LIST_ROWS; i++) browser_draw_row(i);
	} else if (selected != old_selected) {
		browser_draw_row(old_selected - scroll_top);
		browser_draw_row(selected - scroll_top);
	}

	browser_status();

}

static void browser_select(void) {
	if (browser_entry_count() == 0) return;	// nothing to select
	file_ref_t f = browser_entry(selected);
	view_open(f);
}

void browser_yield(vt100_event_t ev) {

	if (ev.type == KEY_UP)   { browser_move(-1); return; }
	if (ev.type == KEY_DOWN) { browser_move(1);  return; }
	if (ev.type == KEY_PGUP) { browser_move(-BROWSER_LIST_ROWS); return; }
	if (ev.type == KEY_PGDN) { browser_move(BROWSER_LIST_ROWS);  return; }

	if (ev.type == KEY_CHAR && ev.ch == CH_CR) {
		browser_select();
		return;
	}

}
