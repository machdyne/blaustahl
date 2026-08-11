#ifndef FLASH_STORAGE_H_
#define FLASH_STORAGE_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Real littlefs-backed flash storage. This is the RP2040-specific half
 * of the storage layer -- storage.c calls these functions for anything
 * involving the flash partition (file listing, reads, whole-file
 * writes, format).
 *
 * NOTE ON TESTING: the mount/format-fallback logic and the file read/
 * write/list wrappers in this file were validated against the real
 * littlefs library using a simulated flash chip (with real NOR erase/
 * program semantics enforced) before being ported here -- see the
 * project notes for that test. What was NOT testable without real
 * hardware is the block-device layer at the very bottom of this file
 * (flash_read/flash_prog/flash_erase) -- the actual flash_range_erase/
 * flash_range_program/XIP calls. Those are written against documented
 * pico-sdk behavior but are the one part of this file that genuinely
 * needs a first real-hardware test before being trusted.
 *
 * Flash is read-only from every editing mode in this firmware (FRAM is
 * the only writable target) -- the one exception is a whole-file write,
 * used only by the FRAM snapshot feature (and, later, file upload).
 * There is no per-byte flash write path, deliberately: littlefs's own
 * caching makes per-byte writes expensive, and nothing in this UI needs
 * them.
 */

void flash_storage_init(void);		// mount, or format+mount if invalid
bool flash_storage_format(void);	// destructive -- wipes everything

int flash_storage_file_count(void);
bool flash_storage_file_info(int idx, char *name_out, int name_out_len,
	uint32_t *size_out);
int flash_storage_file_info_range(int start_idx, int count,
	char names_out[][32], uint32_t *sizes_out);

// direct name-based lookup (lfs_stat), not an index scan -- returns
// false if the file doesn't exist. Used anywhere a caller has a
// filename in hand and needs to know it's real before acting on it
// (e.g. te's fs_size(), and the pre-flight check before ever invoking
// te_edit() at all).
bool flash_storage_file_size(const char *name, uint32_t *size_out);

uint32_t flash_storage_read(const char *name, uint32_t offset, char *buf,
	uint32_t len);
bool flash_storage_write_file(const char *name, const char *data,
	uint32_t len);

// renames/moves a file. If a file already exists at `new_name`, it is
// silently replaced (this is littlefs's own lfs_rename() behavior, not
// something layered on here -- callers that care should check
// flash_storage_file_size(new_name, ...) first and warn/confirm before
// calling this if they want to protect against overwriting).
bool flash_storage_rename(const char *old_name, const char *new_name);

// deletes a file. False if it didn't exist or the delete failed.
bool flash_storage_delete(const char *name);

uint32_t flash_storage_free(void);
uint32_t flash_storage_total(void);

#endif
