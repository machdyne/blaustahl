#ifndef VT100_INPUT_H_
#define VT100_INPUT_H_

typedef enum {
	KEY_NONE = 0,	// mid-sequence, or a swallowed/unrecognized byte
	KEY_CHAR,		// plain character or unhandled control code, see .ch
	KEY_UP,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_PGUP,
	KEY_PGDN,
	KEY_DEL_FWD,	// VT100 "delete" (ESC [ 3 ~)
	KEY_MENU,		// CTRL-T, ESC-ESC, or a lone ESC -- summon/dismiss
					// the menu bar
	KEY_COPY,		// CTRL-C
	KEY_PASTE,		// CTRL-V
	KEY_FILES,		// CTRL-F -- jump directly to the file browser,
					// from any mode (same scope as KEY_MENU)
} vt100_key_t;

typedef struct {
	vt100_key_t type;
	int ch;			// raw byte, valid when type == KEY_CHAR
} vt100_event_t;

/*
 * Feed one raw byte from the serial connection in, get back a logical
 * input event. Shared by every mode (the grid editor, menu bar, and
 * file browser) so VT100 escape-sequence parsing and the menu
 * shortcuts are implemented exactly once, rather than copied into
 * each mode's own input handling.
 */
vt100_event_t vt100_input_feed(int c);

/*
 * Call once per main-loop iteration, BEFORE attempting to read a new
 * byte -- catches a lone ESC that's been waiting long enough that
 * nothing more is coming (as opposed to being followed by another
 * byte, which vt100_input_feed() already resolves synchronously on
 * its own, timeout-free). Returns KEY_MENU if the timeout fired,
 * KEY_NONE otherwise.
 */
vt100_event_t vt100_input_check_timeout(void);

#endif
