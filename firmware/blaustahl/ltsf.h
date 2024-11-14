/* 
 * Long-Term Storage Format
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * This metadata is located in FRAM at address 0x0000 or 0x780 * x.
 *
 */

#include <stdint.h>

#define LTSF_MAGIC 0x1f1e

#define LTSF_ALGO_PLAINTEXT 0
#define LTSF_ALGO_SHA256_CHACHA20_POLY1305 1

typedef struct ltsf_meta_t {

	uint16_t		magic;				// indicates LTSF metadata is present
	uint8_t		version;				// LTSF version (0x00)
	uint8_t		algo;					// LTSF encryption algorithm:
											//  0x00 none / plaintext
											//  0x01 SHA256 KDF + ChaCha20-Poly1305
	uint8_t		plaindesc[48];		// describes content or encryption algorithm
	uint8_t		salt[16];			// salt used by KDF
	uint8_t		nonce[12];			// nonce used by encryption algo
	uint8_t		reserved_a[12];	// reserved
	uint8_t		tag[16];				// MAC
	uint8_t		reserved_b[16];	// reserved
	uint32_t		bootctr;				// boot counter

} ltsf_meta_t;
