/*
 * Real littlefs-backed flash storage for the RP2040.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Partition layout: firmware occupies 0x000000-0x1FFFFF (2MB, generous
 * headroom over the actual firmware size), littlefs occupies
 * 0x200000-0x3FFFFF (the remaining 2MB of the 4MB chip). This boundary
 * must never move without also updating the firmware-size assertion in
 * the build and any tooling that builds a combined firmware+filesystem
 * image (mklittlefs + UF2 concatenation, per the project plan).
 *
 * *** HARDWARE-CRITICAL SAFETY NOTE ***
 * This firmware executes from the same flash chip littlefs writes to
 * (XIP). Erasing/programming while the OTHER core is still fetching
 * instructions from flash will crash the device. In this project, core0
 * runs the USB task loop continuously (blaustahl.c's main()) and core1
 * runs the UI, including the CLI's "format" command and anything that
 * calls flash_storage_write_file() (blaustahl.c's core1_main()). That
 * means core0 is the one that must be paused during a flash write, and
 * core1 is the one initiating it -- the reverse of the usual pico-sdk
 * single-core-does-everything example. This requires a matching
 * one-line change in blaustahl.c's main() (core0's entry point):
 *
 *     multicore_lockout_victim_init();
 *
 * called early, before core1 is launched, so core0 can arm itself as
 * pausable. Without that call, flash_safe_execute() below will not be
 * able to safely pause core0, and a format/snapshot could hang or
 * crash the device.
 *
 * Confirmed working on real hardware. Getting there required tracking
 * down two real bugs beyond the lockout wiring itself, worth recording
 * here since both were silent rather than compile/link errors:
 *  - pico_flash (where flash_safe_execute() lives) was never linked,
 *    only hardware_flash (the raw erase/program calls) was.
 *  - PICO_FLASH_SIZE_BYTES was not declared anywhere in this build
 *    tree, so the SDK fell back to the generic "pico" board's 2MB
 *    default even though this board has 4MB -- meaning every address
 *    in this file's 2MB-4MB partition was out of range as far as the
 *    SDK was concerned. See CMakeLists.txt for both fixes.
 */

#include <string.h>

#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

#include "lfs.h"
#include "flash_storage.h"

#define FLASH_TARGET_OFFSET (2u * 1024u * 1024u)		// 2MB into the chip
#define FS_BLOCK_SIZE  FLASH_SECTOR_SIZE				// 4096 (erase granularity)
#define FS_PROG_SIZE   FLASH_PAGE_SIZE					// 256 (program granularity)
#define FS_BLOCK_COUNT ((2u * 1024u * 1024u) / FS_BLOCK_SIZE)	// 512 (2MB partition)

// If this fails to compile, PICO_FLASH_SIZE_BYTES (from this board's
// pico-sdk board definition, or the CMakeLists.txt override) is
// smaller than expected for this board's actual flash size -- see
// CMakeLists.txt for the real fix if this ever fires again.
_Static_assert(FLASH_TARGET_OFFSET + (FS_BLOCK_COUNT * FS_BLOCK_SIZE)
	<= PICO_FLASH_SIZE_BYTES,
	"littlefs partition extends beyond PICO_FLASH_SIZE_BYTES -- "
	"check this board's flash size definition");

#define FLASH_SAFE_TIMEOUT_MS 1000

static int rp2040_read(const struct lfs_config *c, lfs_block_t block,
		lfs_off_t off, void *buffer, lfs_size_t size) {
	(void)c;
	uint32_t addr = XIP_BASE + FLASH_TARGET_OFFSET + block * FS_BLOCK_SIZE + off;
	memcpy(buffer, (const void *)addr, size);
	return 0;
}

struct prog_params {
	uint32_t addr;
	const uint8_t *data;
	size_t size;
};

static void prog_op(void *param) {
	struct prog_params *p = (struct prog_params *)param;
	flash_range_program(p->addr, p->data, p->size);
}

static int rp2040_prog(const struct lfs_config *c, lfs_block_t block,
		lfs_off_t off, const void *buffer, lfs_size_t size) {
	(void)c;
	struct prog_params p = {
		.addr = FLASH_TARGET_OFFSET + block * FS_BLOCK_SIZE + off,
		.data = buffer,
		.size = size,
	};
	int rc = flash_safe_execute(prog_op, &p, FLASH_SAFE_TIMEOUT_MS);
	return (rc == PICO_OK) ? 0 : LFS_ERR_IO;
}

static void erase_op(void *param) {
	uint32_t addr = *(uint32_t *)param;
	flash_range_erase(addr, FS_BLOCK_SIZE);
}

static int rp2040_erase(const struct lfs_config *c, lfs_block_t block) {
	(void)c;
	uint32_t addr = FLASH_TARGET_OFFSET + block * FS_BLOCK_SIZE;
	int rc = flash_safe_execute(erase_op, &addr, FLASH_SAFE_TIMEOUT_MS);
	return (rc == PICO_OK) ? 0 : LFS_ERR_IO;
}

static int rp2040_sync(const struct lfs_config *c) {
	(void)c;
	return 0;
}

static uint8_t read_buf[FS_PROG_SIZE];
static uint8_t prog_buf[FS_PROG_SIZE];
static uint8_t lookahead_buf[64];

static const struct lfs_config flash_cfg = {
	.read = rp2040_read,
	.prog = rp2040_prog,
	.erase = rp2040_erase,
	.sync = rp2040_sync,

	.read_size = FS_PROG_SIZE,
	.prog_size = FS_PROG_SIZE,
	.block_size = FS_BLOCK_SIZE,
	.block_count = FS_BLOCK_COUNT,
	.block_cycles = 500,

	.cache_size = FS_PROG_SIZE,
	.lookahead_size = sizeof(lookahead_buf),

	.read_buffer = read_buf,
	.prog_buffer = prog_buf,
	.lookahead_buffer = lookahead_buf,
};

/*
 * Everything below this point is the same mount/format-fallback and
 * file read/write/list logic validated against real littlefs with a
 * simulated flash chip -- only the config/callbacks above are
 * RP2040-specific.
 */

static lfs_t lfs;
static bool mounted = false;

void flash_storage_init(void) {

	int err = lfs_mount(&lfs, &flash_cfg);

	if (err) {
		// no valid filesystem found (expected on first boot, or after
		// a format) -- format and mount fresh
		lfs_format(&lfs, &flash_cfg);
		err = lfs_mount(&lfs, &flash_cfg);
	}

	mounted = (err == 0);

}

bool flash_storage_format(void) {

	if (mounted) {
		lfs_unmount(&lfs);
		mounted = false;
	}

	if (lfs_format(&lfs, &flash_cfg) != 0) return false;
	if (lfs_mount(&lfs, &flash_cfg) != 0) return false;

	mounted = true;
	return true;

}

int flash_storage_file_count(void) {

	if (!mounted) return 0;

	lfs_dir_t dir;
	if (lfs_dir_open(&lfs, &dir, "/") != 0) return 0;

	int count = 0;
	struct lfs_info info;

	while (lfs_dir_read(&lfs, &dir, &info) > 0) {
		if (info.type == LFS_TYPE_REG) count++;
	}

	lfs_dir_close(&lfs, &dir);
	return count;

}

bool flash_storage_file_info(int idx, char *name_out, int name_out_len,
		uint32_t *size_out) {

	if (!mounted || idx < 0) return false;

	lfs_dir_t dir;
	if (lfs_dir_open(&lfs, &dir, "/") != 0) return false;

	struct lfs_info info;
	int seen = 0;
	bool found = false;

	while (lfs_dir_read(&lfs, &dir, &info) > 0) {
		if (info.type != LFS_TYPE_REG) continue;
		if (seen == idx) {
			strncpy(name_out, info.name, name_out_len - 1);
			name_out[name_out_len - 1] = 0;
			*size_out = info.size;
			found = true;
			break;
		}
		seen++;
	}

	lfs_dir_close(&lfs, &dir);
	return found;

}

// batch version of flash_storage_file_info: fetches up to `count`
// entries starting at logical index `start_idx` in ONE directory scan,
// instead of one scan per entry (flash_storage_file_info() re-scans
// from the start every single call -- fine for a one-off lookup, but
// real SPI flash read latency makes calling it once per visible row on
// every redraw genuinely slow once there are more than a handful of
// files). names_out/sizes_out are caller-allocated, sized for `count`
// entries each. Returns the number of entries actually filled (can be
// less than `count` if the directory has fewer entries left).
int flash_storage_file_info_range(int start_idx, int count,
		char names_out[][32], uint32_t *sizes_out) {

	if (!mounted || start_idx < 0 || count <= 0) return 0;

	lfs_dir_t dir;
	if (lfs_dir_open(&lfs, &dir, "/") != 0) return 0;

	struct lfs_info info;
	int seen = 0;
	int filled = 0;

	while (filled < count && lfs_dir_read(&lfs, &dir, &info) > 0) {
		if (info.type != LFS_TYPE_REG) continue;
		if (seen < start_idx) { seen++; continue; }
		strncpy(names_out[filled], info.name, 31);
		names_out[filled][31] = 0;
		sizes_out[filled] = info.size;
		filled++;
		seen++;
	}

	lfs_dir_close(&lfs, &dir);
	return filled;

}

bool flash_storage_file_size(const char *name, uint32_t *size_out) {

	if (!mounted) return false;

	struct lfs_info info;
	if (lfs_stat(&lfs, name, &info) != 0) return false;
	if (info.type != LFS_TYPE_REG) return false;

	*size_out = info.size;
	return true;

}

uint32_t flash_storage_read(const char *name, uint32_t offset, char *buf,
		uint32_t len) {

	if (!mounted) return 0;

	lfs_file_t file;
	if (lfs_file_open(&lfs, &file, name, LFS_O_RDONLY) != 0) return 0;

	lfs_file_seek(&lfs, &file, offset, LFS_SEEK_SET);
	lfs_ssize_t got = lfs_file_read(&lfs, &file, buf, len);

	lfs_file_close(&lfs, &file);

	return got > 0 ? (uint32_t)got : 0;

}

bool flash_storage_write_file(const char *name, const char *data,
		uint32_t len) {

	if (!mounted) return false;

	lfs_file_t file;
	int err = lfs_file_open(&lfs, &file, name,
		LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
	if (err != 0) return false;

	lfs_ssize_t written = lfs_file_write(&lfs, &file, data, len);

	lfs_file_close(&lfs, &file);

	return written == (lfs_ssize_t)len;

}

bool flash_storage_rename(const char *old_name, const char *new_name) {
	if (!mounted) return false;
	return lfs_rename(&lfs, old_name, new_name) == 0;
}

bool flash_storage_delete(const char *name) {
	if (!mounted) return false;
	return lfs_remove(&lfs, name) == 0;
}

uint32_t flash_storage_total(void) {
	return (uint32_t)flash_cfg.block_count * flash_cfg.block_size;
}

uint32_t flash_storage_free(void) {

	if (!mounted) return 0;

	lfs_ssize_t used_blocks = lfs_fs_size(&lfs);
	if (used_blocks < 0) return 0;

	uint32_t used = (uint32_t)used_blocks * flash_cfg.block_size;
	uint32_t total = flash_storage_total();

	return used > total ? 0 : total - used;

}
