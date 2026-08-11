/*
 * Streaming file viewer for Blaustahl -- the flash-file counterpart
 * to the grid editor (FRAM/SRAM only). Flash files can be arbitrarily
 * large, so this scrolls one REAL LINE at a time and never loads more
 * than a screen's worth of content into memory. Always read-only --
 * flash files are edited with `te` (CLI), never here.
 *
 * Tracks its own file reference (view_file) rather than sharing
 * editor.c's current_file -- the two are deliberately independent, so
 * switching back and forth between EDIT and VIEW via the menu
 * preserves each one's own position rather than one clobbering the
 * other. current_file remains dedicated to whichever of FRAM/SRAM the
 * grid editor is currently on.
 *
 * Line-boundary algorithm: a display line ends either at a real 0x0A
 * or after COLS (80) printed characters, whichever comes first
 * (matching a plain, familiar terminal word-wrap). Scrolling forward
 * is straightforward -- read up to COLS bytes, stop at the first \n
 * or COLS, whichever comes first.
 *
 * Scrolling BACKWARD is the genuinely tricky part, and an earlier,
 * simpler version of this algorithm shipped with a real bug worth
 * documenting: naively looking backward for the closest \n within the
 * last COLS bytes is NOT the same as finding the previous *real* line.
 * If the byte immediately before the current position happens to be a
 * \n, that's ambiguous on its own -- it could mean "the previous line
 * is just that one byte" (an empty line) or "a much longer real line
 * happens to end right here." Pure local backward-scanning can't tell
 * these apart, and gets it wrong.
 *
 * The fix is to track `real_line_anchor` alongside `top_offset`: the
 * start of the actual real line (bounded by real \n bytes or file
 * start) that top_offset currently falls within. top_offset minus
 * that anchor is always an exact multiple of COLS by construction, so
 * scrolling up within the same real line is simple arithmetic
 * (top_offset - COLS). Only when top_offset equals its own anchor
 * (meaning we're at the very start of the current real line, so the
 * previous display line belongs to a DIFFERENT, earlier real line) do
 * we need an actual backward scan, and even then we're scanning for
 * the previous real line's boundary specifically, not guessing from a
 * single adjacent byte.
 *
 * This was verified with an exhaustive round-trip test (scroll all
 * the way forward through varied content -- short lines, empty lines,
 * long no-newline runs, mixed -- then scroll all the way back up, and
 * confirm the exact same sequence of offsets comes out in reverse)
 * before being wired into this module, including edge cases like
 * files with no trailing newline, files that are a single newline,
 * and many consecutive empty lines.
 *
 * Copying reuses the grid editor's shared copy buffer (editor.c's
 * editor_copy_buffer_* functions) so text copied here can be pasted
 * into FRAM/SRAM in the grid editor, and vice versa. Selection is
 * anchored at whatever line was on top when CTRL-C was first pressed,
 * extended by scrolling, and copied on a second CTRL-C -- unlike the
 * grid editor, this isn't confined to a single page, since there's no
 * page concept here; the copy itself is simply capped (with a clear
 * "TRUNCATED" notice) at the shared buffer's capacity if the selected
 * range is larger.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "blaustahl.h"
#include "vt100.h"
#include "vt100_input.h"
#include "storage.h"
#include "editor.h"
#include "view.h"

#define ROWS 24
#define COLS 80
#define CONTENT_ROWS (ROWS - 1)		// row 24 reserved for status

#define MAX_BACKSCAN 4096	// bounded lookback for a real line with no
							// newline for a very long stretch --
							// degrades gracefully (treats the bound as
							// if it were a line start) rather than
							// scanning an unbounded distance

// the file currently open in the viewer -- see the file-level comment
// for why this is independent of editor.c's current_file. has_file is
// false until the first file is ever opened (fresh boot, or before
// anything's been selected via FILES or the `view` CLI command).
static file_ref_t view_file;
static bool has_file = false;

static long top_offset = 0;
static long real_line_anchor = 0;

static bool copy_mode = false;
static long copy_origin = 0;

static char printable_or_dot(char c) {
	return (c > 0x1f && c < 0x7f) ? c : '.';
}

static bool in_selection(long offset) {
	if (!copy_mode) return false;
	long lo = copy_origin < top_offset ? copy_origin : top_offset;
	long hi = copy_origin < top_offset ? top_offset : copy_origin;
	return offset >= lo && offset < hi;
}

// advances from a line start to the NEXT one -- either right after a
// real \n found within the next COLS bytes, or exactly COLS bytes
// later (soft wrap) if none found, or short of that at EOF. If a real
// \n was crossed, *anchor is updated to the new line start (the byte
// right after it); otherwise *anchor is left unchanged (still within
// the same real line).
static long next_line_start(long offset, long *anchor) {

	char buf[COLS];
	uint32_t got = storage_read(view_file, offset, buf, COLS);

	for (uint32_t i = 0; i < got; i++) {
		if (buf[i] == 0x0a) {
			long new_off = offset + (long)i + 1;
			if (anchor) *anchor = new_off;
			return new_off;
		}
	}

	return offset + (long)got;

}

// draws one display line starting at `offset` onto physical row
// `phys_row`, applying the copy highlight where relevant. Returns the
// offset of the next display line (identical semantics to
// next_line_start(), just with the side effect of actually printing).
static long draw_one_line(long offset, int phys_row) {

	printf(VT100_CURSOR_MOVE_TO, phys_row, 1);

	char buf[COLS];
	uint32_t got = storage_read(view_file, offset, buf, COLS);

	uint32_t i;
	for (i = 0; i < got; i++) {

		if (buf[i] == 0x0a) { i++; break; }

		bool hl = in_selection(offset + (long)i);
		if (hl) printf(VT100_SGR_REVERSE);
		cdc_putchar(printable_or_dot(buf[i]));
		if (hl) printf(VT100_SGR_RESET);

	}

	return offset + (long)i;

}

// scrolls forward one display line. Refuses (returns false, no state
// change) if doing so would leave nothing real to show -- top_offset
// always points at genuine content, never "just past the end."
static bool scroll_down(void) {

	long new_anchor = real_line_anchor;
	long next = next_line_start(top_offset, &new_anchor);

	if (next >= (long)view_file.size) return false;

	top_offset = next;
	real_line_anchor = new_anchor;
	return true;

}

// finds the start of the real line strictly before `before`, which
// must itself already be a real line's start (0, or right after a
// real \n). Scans backward in chunks (not byte-by-byte -- real SPI
// flash read latency makes that meaningfully slower), bounded by
// MAX_BACKSCAN.
static long find_prev_real_line_anchor(long before) {

	if (before <= 0) return 0;

	long search_end = before - 2;	// skip the \n that defines `before` itself --
									// that's not a PREVIOUS boundary, it's the
									// one that already defines this one
	if (search_end < 0) return 0;

	long limit = before - MAX_BACKSCAN;
	if (limit < 0) limit = 0;

	char buf[128];
	long window_end = search_end + 1;	// exclusive

	while (window_end > limit) {

		long window_start = window_end - (long)sizeof(buf);
		if (window_start < limit) window_start = limit;

		uint32_t want = (uint32_t)(window_end - window_start);
		uint32_t got = storage_read(view_file, window_start, buf, want);

		for (int i = (int)got - 1; i >= 0; i--) {
			if (buf[i] == 0x0a) return window_start + (long)i + 1;
		}

		window_end = window_start;

	}

	return limit;

}

// scrolls backward one display line. See the file-level comment for
// why this needs real_line_anchor rather than pure local
// backward-scanning.
static bool scroll_up(void) {

	if (top_offset <= 0) return false;

	long new_top;

	if (top_offset > real_line_anchor) {
		// still within the same real line -- one soft-wrap segment back
		new_top = top_offset - COLS;
	} else {
		// at the start of the current real line -- the previous
		// display line belongs to an earlier real line entirely
		long new_anchor = find_prev_real_line_anchor(real_line_anchor);
		long length = real_line_anchor - new_anchor;
		new_top = new_anchor;
		if (length > 0) new_top = new_anchor + ((length - 1) / COLS) * COLS;
		real_line_anchor = new_anchor;
	}

	top_offset = new_top;
	return true;

}

static void status(void) {

	printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
	printf(VT100_ERASE_LINE);

	if (copy_mode) {
		long lo = copy_origin < top_offset ? copy_origin : top_offset;
		long hi = copy_origin < top_offset ? top_offset : copy_origin;
		printf("BLAUSTAHL -- VIEW -- %s -- COPY (%ld BYTES) -- OFFSET %ld/%u",
			view_file.name, hi - lo, top_offset, view_file.size);
	} else {
		printf("BLAUSTAHL -- VIEW -- %s -- OFFSET %ld/%u",
			view_file.name, top_offset, view_file.size);
	}

	fflush(stdout);

}

void view_redraw(void) {

	printf(VT100_CLEAR_HOME);
	printf(VT100_ERASE_SCREEN);

	if (!has_file) {
		printf(VT100_CURSOR_MOVE_TO, 1, 1);
		printf("NO FILE SELECTED -- PRESS CTRL-F TO BROWSE FILES");
		printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
		printf(VT100_ERASE_LINE);
		printf("BLAUSTAHL -- VIEW -- (NONE)");
		fflush(stdout);
		return;
	}

	long offset = top_offset;

	for (int row = 1; row <= CONTENT_ROWS; row++) {
		offset = draw_one_line(offset, row);
		if (offset >= (long)view_file.size) break;
	}

	status();

}

void view_open(file_ref_t f) {

	view_file = f;
	has_file = true;

	top_offset = 0;
	real_line_anchor = 0;
	copy_mode = false;

	mode = MODE_VIEW;

	view_redraw();

}

void view_return(void) {
	mode = MODE_VIEW;
	view_redraw();
}

bool view_has_file(void) {
	return has_file;
}

file_ref_t view_current_file(void) {
	return view_file;
}

void view_cancel_copy(void) {
	copy_mode = false;
}

static void complete_copy(void) {

	long lo = copy_origin < top_offset ? copy_origin : top_offset;
	long hi = copy_origin < top_offset ? top_offset : copy_origin;
	long len = hi - lo;

	uint32_t cap = editor_copy_buffer_capacity();
	bool truncated = false;
	if (len > (long)cap) { len = (long)cap; truncated = true; }

	static uint8_t tmp[EDITOR_COPY_BUFFER_SIZE];
	uint32_t got = 0;
	if (len > 0) got = storage_read(view_file, lo, (char *)tmp, (uint32_t)len);

	editor_copy_buffer_set(tmp, got);

	copy_mode = false;

	view_redraw();	// clears the highlight

	printf(VT100_CURSOR_MOVE_TO, ROWS, 1);
	printf(VT100_ERASE_LINE);
	printf("BLAUSTAHL -- COPIED %u BYTES%s", got, truncated ? " (TRUNCATED)" : "");
	fflush(stdout);

}

void view_yield(vt100_event_t ev) {

	// CTRL-G (help) works even with nothing loaded yet
	if (ev.type == KEY_CHAR && ev.ch == CH_BEL) {
		editor_help();
		return;
	}

	if (!has_file) return;	// nothing to navigate/copy yet

	if (ev.type == KEY_UP) {
		if (scroll_up()) view_redraw();
		return;
	}

	if (ev.type == KEY_DOWN) {
		if (scroll_down()) view_redraw();
		return;
	}

	if (ev.type == KEY_PGUP) {
		bool moved = false;
		for (int i = 0; i < CONTENT_ROWS; i++) {
			if (!scroll_up()) break;
			moved = true;
		}
		if (moved) view_redraw();
		return;
	}

	if (ev.type == KEY_PGDN) {
		bool moved = false;
		for (int i = 0; i < CONTENT_ROWS; i++) {
			if (!scroll_down()) break;
			moved = true;
		}
		if (moved) view_redraw();
		return;
	}

	if (ev.type == KEY_HOME) {
		if (top_offset != 0) {
			top_offset = 0;
			real_line_anchor = 0;
			view_redraw();
		}
		return;
	}

	if (ev.type == KEY_COPY) {

		if (!copy_mode) {
			copy_mode = true;
			copy_origin = top_offset;
			view_redraw();
			return;
		}

		complete_copy();
		return;

	}

}
