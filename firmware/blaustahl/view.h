#ifndef VIEW_H_
#define VIEW_H_

#include "vt100_input.h"
#include "storage.h"

// opens `f` (must be a flash file) in the viewer, resetting scroll
// position to the top -- for switching to a genuinely different file
// (browser selection, the `view` CLI command).
void view_open(file_ref_t f);

// returns to the viewer for whatever file was already being viewed,
// preserving the current scroll position -- for the menu's VIEW item,
// or anywhere else that just wants to go back to where you were
// rather than open a new file. If no file has ever been viewed this
// session, view_redraw() shows a message rather than reading garbage.
void view_return(void);

// true if a file has ever been opened this session -- lets callers
// (the menu) decide whether VIEW should return here or fall back to
// the file browser.
bool view_has_file(void);

// the file currently being viewed (only meaningful if
// view_has_file() is true) -- used by the browser to preselect the
// right entry when reopening.
file_ref_t view_current_file(void);

// cancels any in-progress copy selection without completing it.
void view_cancel_copy(void);

void view_redraw(void);
void view_yield(vt100_event_t ev);

#endif
