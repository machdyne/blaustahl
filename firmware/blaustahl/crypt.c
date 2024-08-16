/*
 * Blaustahl Utility - Cryptography Functions
 * Copyright (c) 2024 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include <psa/crypto.h>

int crypt_init(psa_key_id_t *key, char *key_bytes) {

	psa_status_t status;

	size_t key_bits = 256;

	status = psa_crypto_init();

	if (status != PSA_SUCCESS) {
		cdc_printf("init fail status: %d\r\n", status);
	}

	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT |
		PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attributes, PSA_ALG_CHACHA20_POLY1305);
	psa_set_key_type(&attributes, PSA_KEY_TYPE_CHACHA20);
	psa_set_key_bits(&attributes, key_bits);

	status = psa_import_key(&attributes, key_bytes, key_bits / 8, key);

	if (status != PSA_SUCCESS) {
		cdc_printf("key import status: %d\r\n", status);
	}

	if (status == PSA_SUCCESS)
		return 1;
	else
		return 0;

}

int crypt_encrypt(psa_key_id_t key, uint8_t *nonce, uint8_t *pos, uint8_t *pt, size_t pt_size, uint8_t *ct, size_t ct_size, size_t *ct_len) {

	psa_status_t status;

	status = psa_aead_encrypt(key,
		PSA_ALG_CHACHA20_POLY1305,
		nonce, 12,
		pos, 4,
		pt, pt_size,
		ct, ct_size, &ct_len);

	if (status != PSA_SUCCESS) {
		cdc_printf("encrypt fail status: %d\r\n", status);
	}

	// cdc_printf("encrypt ct_len: %ld\r\n", ct_len);

	if (status == PSA_SUCCESS)
		return 1;
	else
		return 0;

}

int crypt_decrypt(psa_key_id_t key, uint8_t *nonce, uint8_t *pos, uint8_t *ct, size_t ct_size, uint8_t *pt, size_t pt_size, size_t *pt_len) {

	psa_status_t status;

	status = psa_aead_decrypt(key,
		PSA_ALG_CHACHA20_POLY1305,
		nonce, 12,
		pos, 4,
		ct, ct_size,
		pt, pt_size, &pt_len);

	if (status != PSA_SUCCESS) {
		cdc_printf("decrypt fail status: %d\r\n", status);
	}

	// cdc_printf("decrypt pt_len: %ld\r\n", pt_len);

	if (status == PSA_SUCCESS)
		return 1;
	else
		return 0;

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
