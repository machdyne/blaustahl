/*
 * Shared VT100 input parser.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Turns raw serial bytes into logical key events. Every mode uses this
 * instead of parsing VT100 bytes itself, so cursor keys and control
 * shortcuts behave identically everywhere.
 *
 * The menu opens via CTRL-T, ESC-ESC, or a lone ESC. These collapse
 * into one simple rule rather than two separate mechanisms: once a
 * bare ESC has been seen, ANY next byte other than '[' immediately
 * fires KEY_MENU -- this already covers ESC-ESC (the second ESC is
 * just one case of "not ["), resolved synchronously with no timer
 * involved. A genuine timeout is only needed for the remaining case:
 * ESC arrives and then nothing else does at all (the user paused, or
 * simply meant to press ESC alone). vt100_input_check_timeout() is
 * for exactly that -- call it once per loop iteration, before
 * attempting to read a new byte, so it can fire even when no byte is
 * currently available. Known, accepted tradeoff: if a real keystroke
 * happens to land right after a lone ESC within the timeout window
 * (not '[', not another ESC), that byte is dropped along with opening
 * the menu, rather than queued and reprocessed -- a narrow edge case
 * not worth a pushback-byte mechanism for.
 *
 * Home/End are recognized in all three VT100/xterm/rxvt conventions
 * terminals actually send: ESC[H/ESC[F, ESC[1~/ESC[4~, and ESC[7~/ESC[8~
 * (all three share the tilde-terminated pattern already used for
 * PGUP/PGDN/DEL where relevant).
 */

#include "pico/time.h"

#include "vt100.h"
#include "vt100_input.h"

#define STATE_NONE 0
#define STATE_ESC0 1	// saw ESC, waiting to see what follows
#define STATE_ESC1 2	// saw ESC [, waiting for the final byte
#define STATE_ESC2 3	// swallowing a trailing '~' (e.g. ESC [ 5 ~)

#define ESC_TIMEOUT_US 150000	// generous against both a real arrow-key
								// burst (bytes arrive within the same USB
								// packet, <1ms apart) AND worst-case
								// device-busy time after a heavy redraw
								// (full-page redraws can take a while to
								// transmit if the host's read side lags)

static int state = STATE_NONE;
static absolute_time_t esc_time;

vt100_event_t vt100_input_feed(int c) {

	vt100_event_t ev = { KEY_NONE, 0 };

	if (state == STATE_ESC2) {
		// the action already happened on the digit; this byte (usually
		// '~') is just discarded, whatever it turns out to be
		state = STATE_NONE;
		return ev;
	}

	if (state == STATE_ESC1) {
		state = STATE_NONE;
		switch (c) {
			case 'A': ev.type = KEY_UP;    break;
			case 'B': ev.type = KEY_DOWN;  break;
			case 'C': ev.type = KEY_RIGHT; break;
			case 'D': ev.type = KEY_LEFT;  break;
			case 'H': ev.type = KEY_HOME;  break;	// ESC[H  (VT100/xterm)
			case 'F': ev.type = KEY_END;   break;	// ESC[F  (VT100/xterm)
			case '1': ev.type = KEY_HOME;    state = STATE_ESC2; break; // ESC[1~
			case '4': ev.type = KEY_END;     state = STATE_ESC2; break; // ESC[4~
			case '7': ev.type = KEY_HOME;    state = STATE_ESC2; break; // ESC[7~ (rxvt)
			case '8': ev.type = KEY_END;     state = STATE_ESC2; break; // ESC[8~ (rxvt)
			case '5': ev.type = KEY_PGUP;    state = STATE_ESC2; break;
			case '6': ev.type = KEY_PGDN;    state = STATE_ESC2; break;
			case '3': ev.type = KEY_DEL_FWD; state = STATE_ESC2; break;
			default: break;		// unrecognized final byte, drop it
		}
		return ev;
	}

	if (state == STATE_ESC0) {
		state = STATE_NONE;
		if (c == '[') { state = STATE_ESC1; return ev; }
		// anything else -- including a second ESC -- means the first
		// ESC was standalone: open the menu
		ev.type = KEY_MENU;
		return ev;
	}

	// STATE_NONE

	if (c == CH_ESC) {
		state = STATE_ESC0;
		esc_time = get_absolute_time();
		return ev;
	}
	if (c == CH_DC4) { ev.type = KEY_MENU;  return ev; }	// CTRL-T
	if (c == CH_ETX) { ev.type = KEY_COPY;  return ev; }	// CTRL-C
	if (c == CH_SYN) { ev.type = KEY_PASTE; return ev; }	// CTRL-V
	if (c == CH_ACK) { ev.type = KEY_FILES; return ev; }	// CTRL-F

	ev.type = KEY_CHAR;
	ev.ch = c;
	return ev;

}

vt100_event_t vt100_input_check_timeout(void) {

	vt100_event_t ev = { KEY_NONE, 0 };

	if (state == STATE_ESC0 &&
			absolute_time_diff_us(esc_time, get_absolute_time()) > ESC_TIMEOUT_US) {
		state = STATE_NONE;
		ev.type = KEY_MENU;
	}

	return ev;

}
