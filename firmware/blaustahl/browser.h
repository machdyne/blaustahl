#ifndef BROWSER_H_
#define BROWSER_H_

#include "vt100_input.h"

// resets selection (preselecting whatever view.c is currently
// showing, if anything) and draws the full screen.
void browser_init(void);

void browser_redraw(void);		// redraws without resetting selection
void browser_yield(vt100_event_t ev);

#endif
