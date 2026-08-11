/*
 * Long-Term Storage Format
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * This metadata is located in FRAM immediately after the content
 * region (offset FRAM_AVAILABLE, 128 bytes). Ported from the
 * machdyne/blaustahl feature/encryption branch -- byte layout must
 * stay exactly as documented in ltsf_pack()/ltsf_unpack() in
 * storage.c, which is NOT simply this struct's memory layout (the
 * struct has reserved padding fields for future use that are not
 * currently serialized).
 */

#ifndef LTSF_H_
#define LTSF_H_

#include <stdint.h>

#define LTSF_MAGIC 0x1f1e

#define LTSF_ALGO_PLAINTEXT 0
#define LTSF_ALGO_SHA256_CHACHA20_POLY1305 1

#define LTSF_META_SIZE 128		// reserved region size in FRAM
#define LTSF_PACKED_SIZE 100	// actual bytes used by ltsf_pack()

typedef struct ltsf_meta_t {

	uint16_t	magic;			// indicates LTSF metadata is present
	uint8_t		version;		// LTSF version (0x00)
	uint8_t		algo;			// LTSF encryption algorithm:
								//  0x00 none / plaintext
								//  0x01 SHA256 KDF + ChaCha20-Poly1305
	uint8_t		plaindesc[48];	// describes content or encryption algorithm
								// (always cleartext, even when algo != 0)
	uint8_t		salt[16];		// salt used by KDF
	uint8_t		nonce[12];		// nonce used by encryption algo
	uint8_t		tag[16];		// AEAD authentication tag
	uint32_t	bootctr;		// boot counter (informational only)

} ltsf_meta_t;

#endif
