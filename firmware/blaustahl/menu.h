#ifndef MENU_H_
#define MENU_H_

#include "vt100_input.h"

void menu_open(void);		// remembers current mode, draws the bar over row 1
void menu_cancel(void);	// restores whatever mode was active before menu_open()
void menu_yield(vt100_event_t ev);

#endif
