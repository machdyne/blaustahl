/*
 * ChaCha20-Poly1305 AEAD helpers, via mbedtls's PSA crypto API.
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 *
 * Ported from the machdyne/blaustahl feature/encryption branch, with
 * one real bug fixed in the port: the original crypt_encrypt()/
 * crypt_decrypt() took a `size_t *ct_len`/`*pt_len` out-parameter but
 * then passed `&ct_len`/`&pt_len` to the underlying psa_aead_*() call
 * -- since those parameters are already pointers, that's a
 * pointer-to-pointer mismatch against PSA's `size_t *` parameter. It
 * never mattered in the original because the caller only ever used
 * the value in commented-out debug prints; here it's used for a real
 * length sanity check, so the fix matters.
 */

#include <string.h>

#include "crypt.h"

int crypt_init(psa_key_id_t *key, const uint8_t *key_bytes) {

	psa_status_t status;
	size_t key_bits = 256;

	status = psa_crypto_init();
	if (status != PSA_SUCCESS) return 0;

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attributes,
		PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attributes, PSA_ALG_CHACHA20_POLY1305);
	psa_set_key_type(&attributes, PSA_KEY_TYPE_CHACHA20);
	psa_set_key_bits(&attributes, key_bits);

	status = psa_import_key(&attributes, key_bytes, key_bits / 8, key);

	return status == PSA_SUCCESS ? 1 : 0;

}

int crypt_hash(const uint8_t *data, size_t len, uint8_t *out32) {

	size_t out_len = 0;
	psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256,
		data, len, out32, 32, &out_len);

	return (status == PSA_SUCCESS && out_len == 32) ? 1 : 0;

}

int crypt_kdf(const char *password, const uint8_t *salt, uint8_t *key_out) {

	uint8_t pass_salt[32 + 16];
	memset(pass_salt, 0, sizeof(pass_salt));

	size_t plen = strlen(password);
	if (plen > 32) plen = 32;
	memcpy(pass_salt, password, plen);
	memcpy(&pass_salt[32], salt, 16);

	return crypt_hash(pass_salt, sizeof(pass_salt), key_out);

}

int crypt_encrypt(psa_key_id_t key, const uint8_t *nonce, const uint8_t *aad,
		const uint8_t *pt, size_t pt_size,
		uint8_t *ct, size_t ct_size, size_t *ct_len) {

	psa_status_t status = psa_aead_encrypt(key,
		PSA_ALG_CHACHA20_POLY1305,
		nonce, 12,
		aad, 4,
		pt, pt_size,
		ct, ct_size, ct_len);

	return status == PSA_SUCCESS ? 1 : 0;

}

int crypt_decrypt(psa_key_id_t key, const uint8_t *nonce, const uint8_t *aad,
		const uint8_t *ct, size_t ct_size,
		uint8_t *pt, size_t pt_size, size_t *pt_len) {

	psa_status_t status = psa_aead_decrypt(key,
		PSA_ALG_CHACHA20_POLY1305,
		nonce, 12,
		aad, 4,
		ct, ct_size,
		pt, pt_size, pt_len);

	return status == PSA_SUCCESS ? 1 : 0;

}

void crypt_nonce_inc(uint8_t *nonce) {

	uint32_t a, b, c;

	memcpy(&a, &nonce[8], 4);
	memcpy(&b, &nonce[4], 4);
	memcpy(&c, &nonce[0], 4);

	if (!++a) if (!++b) ++c;

	memcpy(&nonce[8], &a, 4);
	memcpy(&nonce[4], &b, 4);
	memcpy(&nonce[0], &c, 4);

}
