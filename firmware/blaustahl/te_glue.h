#ifndef TE_GLUE_H_
#define TE_GLUE_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Wires machdyne/te (the real text editor, -DEMBEDDED build) to this
 * firmware's flash filesystem only -- FRAM/SRAM are never reachable
 * from te, on purpose (te has no concept of "backend," it just calls
 * fs_size/fs_mallocfile/fs_write_file with a filename; those are
 * implemented here to talk to flash_storage.h directly, not through
 * storage.c's FRAM/SRAM/buffer-mode machinery at all).
 *
 * te_edit() (the real function, from te.c) loads the WHOLE file into
 * a malloc'd buffer with no size limit of its own -- te_glue_edit()
 * below is the actual entry point to call instead, since it enforces
 * the safety check te.c doesn't: refuses (without ever calling
 * te_edit()) an EXISTING file that's larger than can be safely
 * malloc'd on this device. A file that doesn't exist yet is not
 * refused -- that's the normal way to create a new one; te_edit()
 * already starts from an empty document in that case.
 */

typedef enum {
	TE_GLUE_OK = 0,
	TE_GLUE_TOO_LARGE,
} te_glue_result_t;

// does the size/existence check, then calls the real te_edit() if safe.
// Blocking -- te takes over the terminal completely until the user
// quits (Esc :q), same as XMODEM already does for a transfer.
te_glue_result_t te_glue_edit(const char *filename);

// the maximum file size te_glue_edit() will agree to open. Exposed so
// the CLI can report it in an error message.
uint32_t te_glue_max_file_size(void);

#endif
