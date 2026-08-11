#ifndef CRYPT_H_
#define CRYPT_H_

#include <stddef.h>
#include <stdint.h>
#include <psa/crypto.h>

/*
 * ChaCha20-Poly1305 AEAD via mbedtls's PSA crypto API. Ported from the
 * machdyne/blaustahl feature/encryption branch. Deliberately returns
 * status codes only -- no printf/logging of any kind, since this
 * firmware's printf shares a wire with the live VT100 UI (the same
 * lesson learned the hard way with littlefs's default logging macros
 * earlier in this project). Callers (storage.c) decide what, if
 * anything, gets shown to the user.
 */

int crypt_init(psa_key_id_t *key, const uint8_t *key_bytes);

// generic SHA-256, used by storage.c to mix multiple inputs (RNG
// output, the board's unique ID, a timestamp) into a well-diffused
// salt rather than relying on get_rand_32() alone. See the discussion
// in the project notes: pico_rand's xoroshiro128** core is a fast
// statistical PRNG, not a cryptographic one, and its entropy sources
// are explicitly documented as "of varying quality" -- mixing in the
// board's factory-unique ID guarantees no cross-device salt collision
// regardless of RNG quality, independent of whatever pico_rand itself
// turns out to provide.
int crypt_hash(const uint8_t *data, size_t len, uint8_t *out32);

// derives a 32-byte key from SHA256(password || salt). password is
// treated as a C string, up to 32 characters, zero-padded to exactly
// 32 bytes before hashing (so "hi" and "hi" followed by 30 NUL bytes
// hash identically -- this matches the reference implementation's
// fixed-width password field, just derived from a real string instead
// of requiring the caller to pre-pad a 32-byte buffer themselves).
// key_out must have room for 32 bytes.
int crypt_kdf(const char *password, const uint8_t *salt, uint8_t *key_out);

int crypt_encrypt(psa_key_id_t key, const uint8_t *nonce, const uint8_t *aad,
	const uint8_t *pt, size_t pt_size,
	uint8_t *ct, size_t ct_size, size_t *ct_len);

int crypt_decrypt(psa_key_id_t key, const uint8_t *nonce, const uint8_t *aad,
	const uint8_t *ct, size_t ct_size,
	uint8_t *pt, size_t pt_size, size_t *pt_len);

void crypt_nonce_inc(uint8_t *nonce);

#endif
