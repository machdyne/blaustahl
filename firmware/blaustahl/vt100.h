/*
 * Shared VT100 escape sequences and control-character constants.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Deliberately limited to genuine VT100 hardware sequences -- no xterm/ANSI
 * color codes, no Unicode box-drawing. This needs to work correctly on any
 * terminal that speaks VT100: PuTTY, minicom, tio, screen, and real serial
 * terminals with no capability negotiation.
 */

#ifndef VT100_H_
#define VT100_H_

// cursor and screen control

#define VT100_CURSOR_UP			"\e[A"
#define VT100_CURSOR_DOWN			"\e[B"
#define VT100_CURSOR_RIGHT			"\e[C"
#define VT100_CURSOR_LEFT			"\e[D"
#define VT100_CURSOR_HOME			"\e[;H"
#define VT100_CURSOR_MOVE_TO		"\e[%i;%iH"
#define VT100_CURSOR_CRLF			"\e[E"
#define VT100_CLEAR_HOME			"\e[;H"
#define VT100_ERASE_SCREEN			"\e[J"
#define VT100_ERASE_LINE			"\e[K"

// SGR (character attributes) -- 0/1/4/5/7 are genuine VT100 attributes.
// No 30-49 color codes: not real VT100, and not safe to assume on a bare
// serial link.

#define VT100_SGR_RESET				"\e[0m"
#define VT100_SGR_BOLD				"\e[1m"
#define VT100_SGR_UNDERLINE			"\e[4m"
#define VT100_SGR_REVERSE			"\e[7m"

// DECSTBM -- set scrolling region. Reserved for a later phase (native
// scrolling of the file browser's list body without a full redraw).

#define VT100_SET_SCROLL_REGION		"\e[%i;%ir"
#define VT100_RESET_SCROLL_REGION		"\e[r"

// control characters used across the firmware's input handling

#define CH_SOH		0x01	// CTRL-A
#define CH_STX		0x02	// CTRL-B -- toggle buffer mode
#define CH_ETX		0x03	// CTRL-C -- copy
#define CH_ENQ		0x05	// CTRL-E
#define CH_ACK		0x06	// CTRL-F -- jump to FILES
#define CH_BEL		0x07	// CTRL-G
#define CH_BS		0x08
#define CH_LF		0x0a
#define CH_CR		0x0d
#define CH_FF		0x0c	// CTRL-L
#define CH_SO		0x0e	// CTRL-N -- toggle newline rendering (TEXT mode)
#define CH_DLE		0x10
#define CH_DC1		0x11	// CTRL-Q
#define CH_DC3		0x13	// CTRL-S
#define CH_DC4		0x14	// CTRL-T -- menu (also: ESC-ESC, lone ESC)
#define CH_SYN		0x16	// CTRL-V -- paste
#define CH_ETB		0x17	// CTRL-W -- toggle immediate-write, or commit
							// the buffer if buffer mode is active
#define CH_CAN		0x18	// CTRL-X
#define CH_EM		0x19	// CTRL-Y
#define CH_ESC		0x1b
#define CH_DEL		0x7f

#endif
