/*
 * Storage backend.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * FRAM access is real (wraps fram_read/fram_write). SRAM is a fixed-size
 * in-memory scratchpad. Flash access is real too, via flash_storage.c's
 * littlefs backend, but view-only.
 *
 * FRAM encryption (ChaCha20-Poly1305 via crypt.c) is layered entirely
 * inside storage_buffer_enter()/storage_buffer_commit() -- everything
 * else (storage_read/storage_write, the grid engine in editor.c) stays
 * completely unaware encryption exists. Metadata (LTSF format) is
 * cached in RAM after first load, since it's read/checked on nearly
 * every storage_can_write() call.
 */

#include <string.h>

#include "pico/rand.h"
#include "pico/unique_id.h"
#include "pico/time.h"

#include "blaustahl.h"
#include "fram.h"
#include "flash_storage.h"
#include "crypt.h"
#include "ltsf.h"
#include "storage.h"

#define SRAM_DISK_SIZE 7680		// matches FRAM_AVAILABLE for now, by
								// deliberate choice, not by structural
								// necessity -- they're independent constants
static uint8_t sram_disk[SRAM_DISK_SIZE];

file_ref_t current_file;

static bool storage_ready = false;

// mounts (or format-fallback + mounts) flash on first actual use, not
// at boot -- FRAM/SRAM access never calls this, so a flash/littlefs
// problem (however it eventually turns out to manifest) can only ever
// affect FILES/CLI, never plain FRAM/SRAM editing.
static void ensure_storage_ready(void) {
	if (storage_ready) return;
	flash_storage_init();
	storage_ready = true;
}

void storage_init(void) {
	ensure_storage_ready();
}

int storage_file_count(void) {
	ensure_storage_ready();
	return flash_storage_file_count();
}

file_ref_t storage_file_at(int idx) {

	ensure_storage_ready();

	file_ref_t f;
	f.kind = STORAGE_FLASH;
	f.index = idx;
	f.size = 0;
	f.name[0] = 0;

	uint32_t size = 0;
	if (flash_storage_file_info(idx, f.name, STORAGE_NAME_LEN, &size))
		f.size = size;

	return f;

}

int storage_flash_file_range(int start_idx, int count, file_ref_t *out) {

	ensure_storage_ready();

	if (count > STORAGE_BATCH_MAX) count = STORAGE_BATCH_MAX;

	static char names[STORAGE_BATCH_MAX][STORAGE_NAME_LEN];
	static uint32_t sizes[STORAGE_BATCH_MAX];

	int filled = flash_storage_file_info_range(start_idx, count, names, sizes);

	for (int i = 0; i < filled; i++) {
		out[i].kind = STORAGE_FLASH;
		out[i].index = start_idx + i;
		out[i].size = sizes[i];
		strncpy(out[i].name, names[i], STORAGE_NAME_LEN - 1);
		out[i].name[STORAGE_NAME_LEN - 1] = 0;
	}

	return filled;

}

file_ref_t storage_fram_ref(void) {

	file_ref_t f;
	f.kind = STORAGE_FRAM;
	f.index = 0;
	strncpy(f.name, "FRAM", STORAGE_NAME_LEN - 1);
	f.name[STORAGE_NAME_LEN - 1] = 0;
	f.size = FRAM_AVAILABLE;

	return f;

}

file_ref_t storage_sram_ref(void) {

	file_ref_t f;
	f.kind = STORAGE_SRAM;
	f.index = 0;
	strncpy(f.name, "SRAM", STORAGE_NAME_LEN - 1);
	f.name[STORAGE_NAME_LEN - 1] = 0;
	f.size = SRAM_DISK_SIZE;

	return f;

}

uint32_t storage_flash_total(void) {
	ensure_storage_ready();
	return flash_storage_total();
}

void storage_board_unique_id(char *out, int out_len) {

	pico_unique_board_id_t board_id;
	pico_get_unique_board_id(&board_id);

	static const char hex[] = "0123456789ABCDEF";
	int pos = 0;

	for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES && pos + 2 < out_len; i++) {
		out[pos++] = hex[(board_id.id[i] >> 4) & 0xf];
		out[pos++] = hex[board_id.id[i] & 0xf];
	}

	out[pos] = 0;

}

uint32_t storage_flash_free(void) {
	ensure_storage_ready();
	return flash_storage_free();
}

bool storage_flash_file_exists(const char *name) {
	ensure_storage_ready();
	uint32_t size;
	return flash_storage_file_size(name, &size);
}

bool storage_flash_rename(const char *old_name, const char *new_name) {
	ensure_storage_ready();
	return flash_storage_rename(old_name, new_name);
}

bool storage_flash_delete(const char *name) {
	ensure_storage_ready();
	return flash_storage_delete(name);
}

// ---- LTSF metadata (encryption state), cached after first load ----

static ltsf_meta_t meta;
static bool meta_loaded = false;

static void ltsf_load_meta(ltsf_meta_t *m) {

	uint8_t mbuf[LTSF_META_SIZE];
	fram_read((char *)mbuf, FRAM_AVAILABLE, LTSF_META_SIZE);

	memcpy(&m->magic, &mbuf[0], 2);
	memcpy(&m->version, &mbuf[2], 1);
	memcpy(&m->algo, &mbuf[3], 1);
	memset(m->plaindesc, 0, 48);
	memcpy(m->plaindesc, &mbuf[4], 47);
	memcpy(m->salt, &mbuf[52], 16);
	memcpy(m->nonce, &mbuf[68], 12);
	memcpy(m->tag, &mbuf[92], 16);
	memcpy(&m->bootctr, &mbuf[124], 4);

}

static void ltsf_save_meta(const ltsf_meta_t *m) {

	uint8_t mbuf[LTSF_META_SIZE];
	memset(mbuf, 0, sizeof(mbuf));

	memcpy(&mbuf[0], &m->magic, 2);
	memcpy(&mbuf[2], &m->version, 1);
	memcpy(&mbuf[3], &m->algo, 1);
	memcpy(&mbuf[4], m->plaindesc, 48);
	memcpy(&mbuf[52], m->salt, 16);
	memcpy(&mbuf[68], m->nonce, 12);
	memcpy(&mbuf[92], m->tag, 16);
	memcpy(&mbuf[124], &m->bootctr, 4);

	for (int i = 0; i < LTSF_META_SIZE; i++)
		fram_write(FRAM_AVAILABLE + i, mbuf[i]);

}

static void ensure_meta_loaded(void) {

	if (meta_loaded) return;

	ltsf_load_meta(&meta);

	if (meta.magic != LTSF_MAGIC) {
		// no valid LTSF metadata yet (fresh/never-initialized region,
		// or a device that never ran firmware with this feature) --
		// initialize as plaintext, same "detect invalid, fall back to
		// a known default" convention used for the flash filesystem's
		// first-boot format
		memset(&meta, 0, sizeof(meta));
		meta.magic = LTSF_MAGIC;
		meta.version = 0;
		meta.algo = LTSF_ALGO_PLAINTEXT;
		strncpy((char *)meta.plaindesc, "plaintext", sizeof(meta.plaindesc) - 1);
	}

	meta.bootctr += 1;
	ltsf_save_meta(&meta);

	meta_loaded = true;

}

// ---- encryption session state ----

static psa_key_id_t key_id;
static bool crypt_valid = false;

crypt_status_t storage_crypt_status(void) {
	ensure_meta_loaded();
	if (meta.algo == LTSF_ALGO_PLAINTEXT) return CRYPT_PLAINTEXT;
	return crypt_valid ? CRYPT_UNLOCKED : CRYPT_LOCKED;
}

bool storage_can_write(file_ref_t f) {
	if (f.kind == STORAGE_SRAM) return true;
	if (f.kind == STORAGE_FRAM) return storage_crypt_status() != CRYPT_LOCKED;
	return false;
}

// ---- buffer mode state ----

#define WRITE_BUFFER_SIZE 7680		// >= both FRAM_AVAILABLE and SRAM_DISK_SIZE

// TWO independent buffers, one per bufferable storage kind (FRAM and
// SRAM) -- not a single shared buffer keyed to whichever file happens
// to be "current". This is what lets you switch back and forth
// between FRAM and SRAM in the grid editor with unsaved changes
// pending on either or both sides: switching no longer discards
// anything, since each kind's edits live in their own buffer,
// independent of what's currently selected. The combined cost is two
// 7680-byte buffers (15360 bytes total) -- trivial against the
// RP2040's 264KB of RAM, especially next to the 64KB XMODEM staging
// buffer already in use elsewhere in this firmware.
typedef struct {
	uint8_t data[WRITE_BUFFER_SIZE];
	uint32_t len;
	bool active;
	bool dirty;
} write_buffer_t;

static write_buffer_t fram_buffer;
static write_buffer_t sram_buffer;

static write_buffer_t *buffer_for_kind(storage_kind_t kind) {
	if (kind == STORAGE_FRAM) return &fram_buffer;
	if (kind == STORAGE_SRAM) return &sram_buffer;
	return NULL;
}

// shared scratch for ciphertext+tag staging (encrypt output / decrypt
// input) -- static, not stack-allocated, deliberately: core1's stack
// budget is unknown/likely small, and 7680+16 bytes on the stack is a
// real overflow risk. Reused across enable/unlock/disable/buffer
// commit -- never called concurrently (single-threaded core1). Only
// FRAM is ever encrypted, so this stays one shared scratch buffer
// even with two independent write buffers now.
#define CRYPT_SCRATCH_SIZE (FRAM_AVAILABLE + 16)
static uint8_t crypt_scratch[CRYPT_SCRATCH_SIZE];

static const uint8_t crypt_aad[4] = { 0x00, 0x00, 0x00, 0x01 };

// ---- raw (unbuffered) access -- the only functions that ever touch
// the real backends. Used internally, and by storage_read/storage_write
// when there's no active redirect to apply. Deliberately never
// encrypt/decrypt -- that only ever happens as a whole-buffer operation
// in storage_buffer_enter()/commit()/the crypt_* functions below. ----

static uint32_t storage_read_raw(file_ref_t f, uint32_t offset, char *buf, uint32_t len) {

	if (f.kind == STORAGE_FRAM) {
		if (offset >= FRAM_AVAILABLE) return 0;
		if (offset + len > FRAM_AVAILABLE) len = FRAM_AVAILABLE - offset;
		fram_read(buf, (int)offset, (int)len);
		return len;
	}

	if (f.kind == STORAGE_SRAM) {
		if (offset >= SRAM_DISK_SIZE) return 0;
		if (offset + len > SRAM_DISK_SIZE) len = SRAM_DISK_SIZE - offset;
		memcpy(buf, &sram_disk[offset], len);
		return len;
	}

	ensure_storage_ready();
	return flash_storage_read(f.name, offset, buf, len);

}

static bool storage_write_raw(file_ref_t f, uint32_t offset, char c) {

	if (f.kind == STORAGE_FRAM) {
		if (offset >= FRAM_AVAILABLE) return false;
		fram_write((int)offset, (unsigned char)c);
		return true;
	}

	if (f.kind == STORAGE_SRAM) {
		if (offset >= SRAM_DISK_SIZE) return false;
		sram_disk[offset] = (uint8_t)c;
		return true;
	}

	return false;	// flash is never writable byte-at-a-time

}

// ---- public read/write: check for a buffer-mode redirect first ----

uint32_t storage_read(file_ref_t f, uint32_t offset, char *buf, uint32_t len) {

	write_buffer_t *b = buffer_for_kind(f.kind);

	if (b && b->active) {
		if (offset >= b->len) return 0;
		if (offset + len > b->len) len = b->len - offset;
		memcpy(buf, &b->data[offset], len);
		return len;
	}

	return storage_read_raw(f, offset, buf, len);

}

bool storage_write(file_ref_t f, uint32_t offset, char c) {

	if (!storage_can_write(f)) return false;

	write_buffer_t *b = buffer_for_kind(f.kind);

	if (b && b->active) {
		if (offset >= b->len) return false;
		b->data[offset] = (uint8_t)c;
		b->dirty = true;
		return true;
	}

	return storage_write_raw(f, offset, c);

}

// ---- buffer mode control ----
// all operate on "the buffer for current_file.kind" -- editor.c only
// ever asks about whatever file it's currently showing, so this stays
// the same simple, parameterless shape it's always had. FRAM's and
// SRAM's independence is invisible at this layer: it just falls out
// of buffer_for_kind() picking the right one underneath.

bool storage_buffer_active(void) {
	write_buffer_t *b = buffer_for_kind(current_file.kind);
	return b && b->active;
}

bool storage_buffer_dirty(void) {
	write_buffer_t *b = buffer_for_kind(current_file.kind);
	return b && b->active && b->dirty;
}

bool storage_buffer_enter(void) {

	write_buffer_t *b = buffer_for_kind(current_file.kind);
	if (!b) return false;

	if (b->active) return true;
	if (!storage_can_write(current_file)) return false;
	if (current_file.size > WRITE_BUFFER_SIZE) return false;

	if (current_file.kind == STORAGE_FRAM &&
			storage_crypt_status() == CRYPT_UNLOCKED) {

		uint32_t got = storage_read_raw(current_file, 0,
			(char *)crypt_scratch, FRAM_AVAILABLE);
		if (got != FRAM_AVAILABLE) return false;
		memcpy(&crypt_scratch[FRAM_AVAILABLE], meta.tag, 16);

		size_t pt_len = 0;
		if (!crypt_decrypt(key_id, meta.nonce, crypt_aad,
				crypt_scratch, FRAM_AVAILABLE + 16,
				b->data, WRITE_BUFFER_SIZE, &pt_len))
			return false;
		if (pt_len != FRAM_AVAILABLE) return false;

		b->len = (uint32_t)pt_len;

	} else {
		b->len = storage_read_raw(current_file, 0,
			(char *)b->data, current_file.size);
	}

	b->active = true;
	b->dirty = false;

	return true;

}

bool storage_buffer_commit(void) {

	write_buffer_t *b = buffer_for_kind(current_file.kind);
	if (!b || !b->active) return false;

	if (current_file.kind == STORAGE_FRAM &&
			storage_crypt_status() == CRYPT_UNLOCKED) {

		crypt_nonce_inc(meta.nonce);

		size_t ct_len = 0;
		if (!crypt_encrypt(key_id, meta.nonce, crypt_aad,
				b->data, b->len,
				crypt_scratch, sizeof(crypt_scratch), &ct_len))
			return false;
		if (ct_len != b->len + 16) return false;

		for (uint32_t i = 0; i < b->len; i++)
			if (!storage_write_raw(current_file, i, (char)crypt_scratch[i]))
				return false;

		memcpy(meta.tag, &crypt_scratch[b->len], 16);
		ltsf_save_meta(&meta);

	} else {
		for (uint32_t i = 0; i < b->len; i++) {
			if (!storage_write_raw(current_file, i, (char)b->data[i]))
				return false;
		}
	}

	b->dirty = false;
	return true;

}

bool storage_buffer_exit(void) {

	write_buffer_t *b = buffer_for_kind(current_file.kind);
	if (!b || !b->active) return true;
	if (b->dirty) return false;

	if (current_file.kind == STORAGE_FRAM &&
			storage_crypt_status() != CRYPT_PLAINTEXT)
		return false;	// buffer mode is not optional for encrypted FRAM

	b->active = false;
	return true;

}

// ---- file selection ----

bool storage_select(file_ref_t f) {

	// each storage kind keeps its own independent buffer, so
	// switching between FRAM and SRAM never discards anything --
	// unlike the old single shared buffer, there's no longer anything
	// here that can fail. Still returns bool: a stable signature for
	// callers, in case a future failure mode is ever added.
	current_file = f;

	if (f.kind == STORAGE_FRAM && storage_crypt_status() == CRYPT_UNLOCKED)
		storage_buffer_enter();	// no-op if FRAM's buffer is already active

	return true;

}

// ---- FRAM encryption: enable / unlock / disable ----

// mixes board-unique-ID + RNG + a timestamp through SHA-256, rather
// than using get_rand_32() output directly as the salt. pico_rand's
// core is xoroshiro128** (a fast statistical PRNG, not a
// cryptographic one) fed by entropy sources the SDK's own docs
// describe as "of varying quality" -- mixing in the board's
// factory-fused unique ID guarantees no cross-device salt
// collision regardless of how that entropy actually turns out,
// independent of trusting pico_rand's own quality. Shared by
// storage_crypt_enable() and storage_crypt_change_password(), since
// both need a genuinely fresh salt.
static bool generate_new_salt(uint8_t salt_out[16]) {

	pico_unique_board_id_t board_id;
	pico_get_unique_board_id(&board_id);

	uint8_t salt_material[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 16 + 8];
	memcpy(&salt_material[0], board_id.id, PICO_UNIQUE_BOARD_ID_SIZE_BYTES);

	uint32_t r;
	r = get_rand_32(); memcpy(&salt_material[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 0],  &r, 4);
	r = get_rand_32(); memcpy(&salt_material[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 4],  &r, 4);
	r = get_rand_32(); memcpy(&salt_material[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 8],  &r, 4);
	r = get_rand_32(); memcpy(&salt_material[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 12], &r, 4);

	uint64_t now = to_us_since_boot(get_absolute_time());
	memcpy(&salt_material[PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 16], &now, 8);

	uint8_t salt_hash[32];
	if (!crypt_hash(salt_material, sizeof(salt_material), salt_hash)) return false;

	memcpy(salt_out, salt_hash, 16);
	return true;

}

bool storage_crypt_enable(const char *password) {

	ensure_meta_loaded();

	if (meta.algo != LTSF_ALGO_PLAINTEXT) return false;
	if (!password || !password[0]) return false;

	// this reads RAW FRAM below, bypassing any buffer -- refuse rather
	// than silently discarding unsaved edits sitting in a dirty buffer
	if (fram_buffer.active && fram_buffer.dirty) return false;

	uint8_t new_salt[16];
	if (!generate_new_salt(new_salt)) return false;

	uint8_t derived_key[32];
	if (!crypt_kdf(password, new_salt, derived_key)) return false;

	psa_key_id_t new_key_id;
	if (!crypt_init(&new_key_id, derived_key)) return false;

	file_ref_t fram = storage_fram_ref();
	static uint8_t plaintext[FRAM_AVAILABLE];
	uint32_t got = storage_read_raw(fram, 0, (char *)plaintext, FRAM_AVAILABLE);
	if (got != FRAM_AVAILABLE) return false;

	uint8_t new_nonce[12];
	memset(new_nonce, 0, 12);

	size_t ct_len = 0;
	if (!crypt_encrypt(new_key_id, new_nonce, crypt_aad,
			plaintext, FRAM_AVAILABLE,
			crypt_scratch, sizeof(crypt_scratch), &ct_len))
		return false;
	if (ct_len != FRAM_AVAILABLE + 16) return false;

	for (uint32_t i = 0; i < FRAM_AVAILABLE; i++)
		if (!storage_write_raw(fram, i, (char)crypt_scratch[i])) return false;

	memcpy(meta.salt, new_salt, 16);
	memcpy(meta.nonce, new_nonce, 12);
	memcpy(meta.tag, &crypt_scratch[FRAM_AVAILABLE], 16);
	meta.magic = LTSF_MAGIC;
	meta.version = 0;
	meta.algo = LTSF_ALGO_SHA256_CHACHA20_POLY1305;
	strncpy((char *)meta.plaindesc, "SHA256(p||salt)+ChaCha20-Poly1305",
		sizeof(meta.plaindesc) - 1);
	ltsf_save_meta(&meta);

	key_id = new_key_id;
	crypt_valid = true;

	// FRAM's buffer (not "whatever current_file.kind happens to be" --
	// FRAM's buffer can be active while SRAM is currently selected)
	// held plaintext keyed to the OLD (nonexistent) encryption state;
	// force a fresh enter so it picks up the newly-encrypted content
	// correctly. Only re-enters immediately if FRAM is what's actually
	// selected right now; otherwise it'll enter next time FRAM is.
	fram_buffer.active = false;
	if (current_file.kind == STORAGE_FRAM) storage_buffer_enter();

	return true;

}

bool storage_crypt_unlock(const char *password) {

	ensure_meta_loaded();

	if (meta.algo == LTSF_ALGO_PLAINTEXT) return false;
	if (!password || !password[0]) return false;

	uint8_t derived_key[32];
	if (!crypt_kdf(password, meta.salt, derived_key)) return false;

	psa_key_id_t new_key_id;
	if (!crypt_init(&new_key_id, derived_key)) return false;

	// verify the password by attempting a real decrypt (AEAD tag
	// check) -- this is the ONLY password verification mechanism;
	// there is no separate stored password hash
	file_ref_t fram = storage_fram_ref();
	uint32_t got = storage_read_raw(fram, 0, (char *)crypt_scratch, FRAM_AVAILABLE);
	if (got != FRAM_AVAILABLE) return false;
	memcpy(&crypt_scratch[FRAM_AVAILABLE], meta.tag, 16);

	static uint8_t scratch_pt[FRAM_AVAILABLE];
	size_t pt_len = 0;
	if (!crypt_decrypt(new_key_id, meta.nonce, crypt_aad,
			crypt_scratch, FRAM_AVAILABLE + 16,
			scratch_pt, FRAM_AVAILABLE, &pt_len))
		return false;
	if (pt_len != FRAM_AVAILABLE) return false;

	key_id = new_key_id;
	crypt_valid = true;

	if (current_file.kind == STORAGE_FRAM) storage_buffer_enter();

	return true;

}

bool storage_crypt_change_password(const char *new_password) {

	if (storage_crypt_status() != CRYPT_UNLOCKED) return false;
	if (!new_password || !new_password[0]) return false;

	// this reads RAW FRAM below, bypassing any buffer -- refuse rather
	// than silently discarding unsaved edits sitting in a dirty buffer
	if (fram_buffer.active && fram_buffer.dirty) return false;

	// decrypt the CURRENT content with the CURRENT key -- a fresh,
	// independent decrypt, not relying on buffer_mode's ambient state
	// (matches storage_crypt_disable()'s own pattern). The plaintext
	// this produces only ever lives in this local RAM buffer -- it is
	// never written to FRAM or flash at any point during rotation.
	file_ref_t fram = storage_fram_ref();
	uint32_t got = storage_read_raw(fram, 0, (char *)crypt_scratch, FRAM_AVAILABLE);
	if (got != FRAM_AVAILABLE) return false;
	memcpy(&crypt_scratch[FRAM_AVAILABLE], meta.tag, 16);

	static uint8_t plaintext[FRAM_AVAILABLE];
	size_t pt_len = 0;
	if (!crypt_decrypt(key_id, meta.nonce, crypt_aad,
			crypt_scratch, FRAM_AVAILABLE + 16,
			plaintext, FRAM_AVAILABLE, &pt_len))
		return false;
	if (pt_len != FRAM_AVAILABLE) return false;

	// derive a NEW key from a genuinely fresh salt (same generation
	// as enabling encryption from scratch)
	uint8_t new_salt[16];
	if (!generate_new_salt(new_salt)) return false;

	uint8_t derived_key[32];
	if (!crypt_kdf(new_password, new_salt, derived_key)) return false;

	psa_key_id_t new_key_id;
	if (!crypt_init(&new_key_id, derived_key)) return false;

	uint8_t new_nonce[12];
	memset(new_nonce, 0, 12);

	// re-encrypt the SAME plaintext with the new key -- straight back
	// to FRAM as ciphertext, still never touching flash/FRAM in
	// plaintext form
	size_t ct_len = 0;
	if (!crypt_encrypt(new_key_id, new_nonce, crypt_aad,
			plaintext, FRAM_AVAILABLE,
			crypt_scratch, sizeof(crypt_scratch), &ct_len))
		return false;
	if (ct_len != FRAM_AVAILABLE + 16) return false;

	for (uint32_t i = 0; i < FRAM_AVAILABLE; i++)
		if (!storage_write_raw(fram, i, (char)crypt_scratch[i])) return false;

	memcpy(meta.salt, new_salt, 16);
	memcpy(meta.nonce, new_nonce, 12);
	memcpy(meta.tag, &crypt_scratch[FRAM_AVAILABLE], 16);
	ltsf_save_meta(&meta);

	key_id = new_key_id;
	// crypt_valid stays true -- still unlocked, just re-keyed

	// content is unchanged (only the key changed), but refresh FRAM's
	// buffer specifically (not "whatever current_file.kind happens to
	// be" -- FRAM's buffer can be active while SRAM is currently
	// selected) if it was active, so nothing stale lingers
	if (fram_buffer.active) {
		fram_buffer.len = FRAM_AVAILABLE;
		memcpy(fram_buffer.data, plaintext, FRAM_AVAILABLE);
		fram_buffer.dirty = false;
	}

	return true;

}

bool storage_crypt_disable(void) {

	if (storage_crypt_status() != CRYPT_UNLOCKED) return false;

	// this reads RAW FRAM below, bypassing any buffer -- refuse rather
	// than silently discarding unsaved edits sitting in a dirty buffer
	if (fram_buffer.active && fram_buffer.dirty) return false;

	file_ref_t fram = storage_fram_ref();
	uint32_t got = storage_read_raw(fram, 0, (char *)crypt_scratch, FRAM_AVAILABLE);
	if (got != FRAM_AVAILABLE) return false;
	memcpy(&crypt_scratch[FRAM_AVAILABLE], meta.tag, 16);

	static uint8_t plaintext[FRAM_AVAILABLE];
	size_t pt_len = 0;
	if (!crypt_decrypt(key_id, meta.nonce, crypt_aad,
			crypt_scratch, FRAM_AVAILABLE + 16,
			plaintext, FRAM_AVAILABLE, &pt_len))
		return false;
	if (pt_len != FRAM_AVAILABLE) return false;

	for (uint32_t i = 0; i < FRAM_AVAILABLE; i++)
		if (!storage_write_raw(fram, i, (char)plaintext[i])) return false;

	meta.algo = LTSF_ALGO_PLAINTEXT;
	strncpy((char *)meta.plaindesc, "plaintext", sizeof(meta.plaindesc) - 1);
	ltsf_save_meta(&meta);

	crypt_valid = false;

	// refresh FRAM's buffer specifically (not "whatever
	// current_file.kind happens to be" -- FRAM's buffer can be active
	// while SRAM is currently selected) to match what's now on disk,
	// if it was active
	if (fram_buffer.active) {
		fram_buffer.len = FRAM_AVAILABLE;
		memcpy(fram_buffer.data, plaintext, FRAM_AVAILABLE);
		fram_buffer.dirty = false;
	}

	return true;

}

// ---- snapshot / format ----

bool storage_snapshot_fram(void) {

	// deliberately storage_read_raw(), not storage_read() -- a
	// snapshot captures what's actually durably stored in FRAM, which
	// is ciphertext if FRAM is encrypted. This never decrypts for a
	// snapshot, on purpose: flash is unencrypted storage, so leaking
	// plaintext there would defeat the point of encrypting FRAM at all.
	static char buf[FRAM_AVAILABLE];

	file_ref_t fram = storage_fram_ref();
	uint32_t got = storage_read_raw(fram, 0, buf, FRAM_AVAILABLE);
	if (got != FRAM_AVAILABLE) return false;

	ensure_storage_ready();
	return flash_storage_write_file("fram_snapshot.bin", buf, FRAM_AVAILABLE);

}

bool storage_format_flash(void) {
	// deliberately does NOT call ensure_storage_ready() first --
	// flash_storage_format() does its own unmount/format/mount
	// sequence and doesn't need (or want) a prior mount attempt that
	// could itself get stuck before format is even tried
	bool ok = flash_storage_format();
	if (ok) storage_ready = true;
	return ok;
}
