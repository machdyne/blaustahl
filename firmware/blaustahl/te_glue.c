/*
 * te (machdyne/te) integration -- flash files only.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blaustahl.h"
#include "flash_storage.h"
#include "storage.h"
#include "te_glue.h"

// te_load() (in te.c) mallocs the WHOLE file in one block, with no
// size limit of its own -- this is the actual ceiling. Chosen
// conservatively: several large static buffers already exist
// elsewhere in this firmware (XMODEM's staging buffer, the write/
// crypt buffers, littlefs's own state), so this leaves real headroom
// rather than assuming te gets the whole of whatever's left. Easy to
// raise later once real hardware free-RAM measurement is available --
// this number is a considered starting point, not a hard architectural
// limit.
#define TE_GLUE_MAX_FILE_SIZE (32u * 1024u)

uint32_t te_glue_max_file_size(void) {
	return TE_GLUE_MAX_FILE_SIZE;
}

te_glue_result_t te_glue_edit(const char *filename) {

	storage_init();	// ensure flash is mounted -- same lazy-init path
						// every other flash-touching action uses

	uint32_t size = 0;
	bool exists = flash_storage_file_size(filename, &size);

	// a file that doesn't exist yet just means "start a new, empty
	// file" -- not an error. te_init() (inside te_edit(), via te.c)
	// already sets up an empty document unconditionally; te_load()
	// correctly does nothing when there's nothing to load rather than
	// failing, so there's nothing more this needs to do for that case.
	// Only an EXISTING file that's too large to load is actually
	// refused here.
	if (exists && size > TE_GLUE_MAX_FILE_SIZE)
		return TE_GLUE_TOO_LARGE;

	// te_edit() takes a non-const char* (it just stores/reuses the
	// pointer internally, never writes through it, but a local mutable
	// copy avoids casting away const to satisfy that signature)
	char name_buf[STORAGE_NAME_LEN];
	strncpy(name_buf, filename, sizeof(name_buf) - 1);
	name_buf[sizeof(name_buf) - 1] = 0;

	extern void te_edit(char *filename);
	te_edit(name_buf);

	return TE_GLUE_OK;

}

// ---- glue functions te.c calls directly (see include/fs.h) ----

int fs_size(char *filename) {
	uint32_t size = 0;
	if (!flash_storage_file_size(filename, &size)) return 0;
	return (int)size;
}

char *fs_mallocfile(char *filename) {

	uint32_t size = 0;
	if (!flash_storage_file_size(filename, &size)) return NULL;

	char *buf = malloc(size);
	if (!buf) return NULL;

	uint32_t got = flash_storage_read(filename, 0, buf, size);
	if (got != size) {
		free(buf);
		return NULL;
	}

	return buf;

}

int fs_write_file(char *filename, char *buf, int len) {
	if (!flash_storage_write_file(filename, buf, (uint32_t)len)) return 0;
	return len;
}

// the single, shared getch() implementation for BOTH te.c and ms.c
// (declared once in fs.h, called by both under their respective
// embedded build flags) -- must be defined exactly once across the
// whole binary; ms_glue.c deliberately does NOT define its own copy
int getch(void) {
	// blocking, by design -- te_edit() runs its own internal
	// while(te_yield()) loop and expects to own the terminal
	// completely until the user quits, the same way XMODEM already
	// blocks core1 for the duration of a transfer
	int c;
	while ((c = cdc_getchar()) == EOF) { }
	return c;
}
