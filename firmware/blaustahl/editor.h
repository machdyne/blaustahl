#ifndef EDITOR_H_
#define EDITOR_H_

#include <stdint.h>

// application modes -- shared with menu.c/browser.c

#define MODE_GRID  1	// viewer/editor for current_file (TEXT or HEX render) -- FRAM/SRAM only
#define MODE_HELP  2
#define MODE_MENU  3	// top menu bar overlay
#define MODE_FILES 4	// file browser
#define MODE_CLI   5	// single-line command CLI
#define MODE_VIEW  6	// streaming line-by-line viewer for flash files (view.c)

extern int mode;

void editor_init(void);
void editor_yield(void);
void editor_redraw(void);
void editor_status(void);

// render style preference for MODE_GRID (TEXT/HEX) -- these ONLY set
// the preference, they do NOT switch mode or redraw. The menu bar's
// MODE group lets you change this while still in the menu; use
// editor_return_to_grid() to actually switch to MODE_GRID afterward.
void editor_set_render_text(void);
void editor_set_render_hex(void);
int editor_is_hex(void);

// returns to MODE_GRID for current_file, preserving cursor_offset
// (unlike editor_open_current_file(), which resets it -- appropriate
// there since that's for switching to a genuinely different file, but
// wrong for the menu's EDIT item, which just returns to wherever you
// already were).
void editor_return_to_grid(void);

// enters MODE_GRID for whatever current_file now is, resetting cursor
// position to the start (a new file has no relationship to the old
// cursor offset) but keeping whichever render (TEXT/HEX) was last
// chosen -- that's a standing preference, not a per-file property.
void editor_open_current_file(void);

// shared copy buffer, used by both the grid editor and cat.c so
// copying in one and pasting in the other works on the same
// underlying storage rather than two independent buffers.
#define EDITOR_COPY_BUFFER_SIZE 2048
uint32_t editor_copy_buffer_capacity(void);
void editor_copy_buffer_set(const uint8_t *data, uint32_t len);
uint32_t editor_copy_buffer_get(uint8_t *out, uint32_t max_len);
uint32_t editor_copy_buffer_len(void);

// enters MODE_HELP. Any keypress returns to whichever mode was
// active before help was triggered (tracked internally). Called by
// CTRL-G and by the menu bar's HELP item.
void editor_help(void);
int editor_mode_before_help(void);

// called by srwp.c after a raw FRAM write -- SRWP operates directly on
// raw FRAM, completely bypassing the grid editor's buffer (see
// storage.c's buffer mode) and any encryption. If the editor has an
// active FRAM buffer -- whether from active editing, or, when FRAM is
// encrypted, simply because encrypted FRAM always requires one to be
// readable at all -- that buffer is now silently stale relative to
// what's actually on the chip. This doesn't correct the buffer itself
// (SRWP has no reason to know or care whether one exists), it just
// arms a one-time warning on the next status line render, the same
// "show once, then clear" pattern as the first-boot hint.
void editor_notify_srwp_write(void);

#endif
