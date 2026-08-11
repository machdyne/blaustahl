#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Unified storage interface. FRAM and SRAM are both real, always-on,
 * always-writable backends (FRAM's writability additionally depends on
 * encryption lock state -- see below). Flash is real too, via
 * flash_storage.c's littlefs backend, but view-only from every editing
 * mode -- there is no per-byte flash write path, only the whole-file
 * snapshot/upload path.
 *
 * FRAM encryption: ChaCha20-Poly1305 (via crypt.c/mbedtls PSA), applied
 * only to FRAM -- SRAM is ephemeral (lost on power-cycle) so there is
 * nothing durable to protect, and flash is never writable at all.
 * Encryption is inseparable from buffer mode: AEAD ciphers authenticate
 * the whole message in one pass, so there is no way to edit a single
 * byte in place without re-processing (and re-authenticating) the
 * entire content -- buffer mode for encrypted FRAM is therefore
 * mandatory, not optional (storage_buffer_exit() refuses while FRAM is
 * encrypted, matching the reference machdyne/blaustahl encryption
 * branch's behavior). Password entry itself is a CLI concern
 * (cli.c's "password"/"disable_encryption" commands), not something
 * this header exposes UI for.
 *
 * Buffer mode is a global, backend-agnostic write policy: instead of
 * writing each keystroke straight to the backend, edits accumulate in
 * one shared RAM buffer and are committed as a whole via
 * storage_buffer_commit(). It applies to whichever file is currently
 * writable (FRAM or SRAM).
 */

#define STORAGE_NAME_LEN 32
#define STORAGE_BATCH_MAX 32	// generous upper bound for storage_flash_file_range()'s
								// caller-allocated arrays -- comfortably covers a
								// full screen's worth of browser rows in one call

typedef enum {
	STORAGE_FRAM = 0,
	STORAGE_SRAM = 1,
	STORAGE_FLASH = 2,
} storage_kind_t;

typedef struct {
	storage_kind_t kind;
	int index;				// position at last enumeration (flash) or
							// unused (FRAM/SRAM) -- NOT a stable
							// identity for flash files across
							// directory mutations; compare by .name
	char name[STORAGE_NAME_LEN];
	uint32_t size;
} file_ref_t;

void storage_init(void);				// optional explicit early mount --
										// nothing calls this automatically;
										// flash mounts lazily on first
										// actual use instead (FILES/CLI),
										// so FRAM/SRAM editing never
										// depends on flash/littlefs
										// succeeding

int storage_file_count(void);			// flash files only, FRAM/SRAM are separate
file_ref_t storage_file_at(int idx);	// idx into the flash file list

// batch version of storage_file_at(): fetches up to `count` entries
// starting at `start_idx` in one directory scan (see the comment on
// flash_storage_file_info_range() -- calling storage_file_at() once
// per row is measurably slow on real flash once there are more than a
// handful of files, since each call re-scans the directory from the
// start). `out` must have room for `count` entries, count <=
// STORAGE_BATCH_MAX. Returns the number of entries actually filled.
int storage_flash_file_range(int start_idx, int count, file_ref_t *out);

file_ref_t storage_fram_ref(void);		// the pinned FRAM pseudo-file
file_ref_t storage_sram_ref(void);		// the pinned SRAM pseudo-file
uint32_t storage_flash_free(void);
uint32_t storage_flash_total(void);

// formats the RP2040's factory-fused unique board ID as a hex string
// (e.g. "E6614C775B303234") into out_len (must be at least
// 2*PICO_UNIQUE_BOARD_ID_SIZE_BYTES+1 = 17 bytes). This is the same
// ID already used as part of FRAM encryption salt generation --
// exposed here read-only, for display in `info`.
void storage_board_unique_id(char *out, int out_len);

// true if a flash file with this exact name exists.
bool storage_flash_file_exists(const char *name);

// renames a flash file. Fails if old_name doesn't exist. Silently
// replaces new_name if it already exists (this is littlefs's own
// behavior) -- callers that want to warn/confirm before overwriting
// should check storage_flash_file_exists(new_name) themselves first.
bool storage_flash_rename(const char *old_name, const char *new_name);

// deletes a flash file. False if it didn't exist.
bool storage_flash_delete(const char *name);

// the globally selected "current file", shared across every mode.
// Returns false (refuses) if buffer mode is active with unsaved
// changes -- commit (storage_buffer_commit) or cleanly exit
// (storage_buffer_exit) first. Switching TO unlocked encrypted FRAM
// automatically (re-)enters buffer mode, decrypting as it does.
extern file_ref_t current_file;
bool storage_select(file_ref_t f);

// unified byte-level access. Returns bytes actually read/written.
// Transparently redirects to the buffer-mode RAM copy when buffer
// mode is active and f is the current writable file.
uint32_t storage_read(file_ref_t f, uint32_t offset, char *buf, uint32_t len);
bool storage_write(file_ref_t f, uint32_t offset, char c);

// true for SRAM always; true for FRAM unless it's encrypted and not
// yet unlocked this session (storage_crypt_status() == CRYPT_LOCKED);
// false for flash always.
bool storage_can_write(file_ref_t f);

// buffer mode
bool storage_buffer_active(void);
bool storage_buffer_dirty(void);		// true only while active AND unsaved
bool storage_buffer_enter(void);		// false if current_file isn't writable
bool storage_buffer_commit(void);		// false on write failure; stays active
bool storage_buffer_exit(void);		// false (refuses) if dirty, or if
										// current_file is encrypted FRAM
										// (buffer mode isn't optional there)

// ---- FRAM encryption ----

typedef enum {
	CRYPT_PLAINTEXT = 0,	// FRAM is not encrypted
	CRYPT_LOCKED = 1,		// FRAM is encrypted, correct password not yet entered
	CRYPT_UNLOCKED = 2,		// FRAM is encrypted and this session has the key
} crypt_status_t;

crypt_status_t storage_crypt_status(void);

// enable encryption on currently-plaintext FRAM: generates a new salt,
// derives a key from `password` (max 32 chars), encrypts the current
// FRAM content, and commits it. False on failure (already encrypted,
// or a crypto/write error) -- FRAM is left untouched on failure.
bool storage_crypt_enable(const char *password);

// attempt to unlock already-encrypted FRAM with `password`. False if
// the password is wrong (AEAD tag check fails) or FRAM isn't
// encrypted at all.
bool storage_crypt_unlock(const char *password);

// changes the password on already-unlocked, encrypted FRAM: decrypts
// with the current key, generates a genuinely fresh salt, derives a
// new key from `new_password`, and re-encrypts. Requires
// CRYPT_UNLOCKED first (i.e. the correct current password must have
// already been entered this session via storage_crypt_unlock()).
// Plaintext content only ever exists in a local RAM buffer during
// rotation -- it is never written to FRAM or flash at any point,
// unlike the disable-then-re-enable workaround this replaces.
bool storage_crypt_change_password(const char *new_password);

// decrypt and save FRAM as plaintext. Requires CRYPT_UNLOCKED first.
bool storage_crypt_disable(void);

// writes a full copy of current FRAM contents to a new flash file
// (fixed name "fram_snapshot.bin" -- overwrites any previous snapshot).
// Always captures FRAM's actual durable content, even if buffer mode
// is active with unsaved edits -- a snapshot is a backup of what's
// really stored, not of in-progress, uncommitted changes. If FRAM is
// encrypted, the snapshot is the ciphertext -- storage_snapshot_fram()
// never has access to plaintext beyond what's already unlocked in RAM,
// and deliberately doesn't try to decrypt for the snapshot regardless.
bool storage_snapshot_fram(void);

// destructive: wipes the entire flash filesystem
bool storage_format_flash(void);

#endif
