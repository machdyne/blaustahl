#ifndef CLI_H_
#define CLI_H_

#include "vt100_input.h"

/*
 * Single-line command CLI, reachable from the menu bar. Built for
 * infrequent system commands (format, ls, info) -- not a shell, not
 * scriptable. If a language interpreter (BASIC/Scheme) gets added
 * later, this line-input loop is the natural place for it to plug in;
 * cli_dispatch()'s command table is deliberately a simple string match
 * so that's a small extension, not a rewrite.
 */

void cli_init(void);
void cli_yield(vt100_event_t ev);
void cli_redraw(void);			// redraw without resetting the session
									// (used when returning from the menu)
void cli_cancel_pending(void);	// defuses a pending destructive confirm
									// (e.g. mid-"format") if the user
									// navigates away instead of answering

#endif
