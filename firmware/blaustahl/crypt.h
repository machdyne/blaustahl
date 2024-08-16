#include <psa/crypto.h>

int crypt_init(psa_key_id_t *key, char *key_bytes);
int crypt_encrypt(psa_key_id_t key, uint8_t *nonce, uint8_t *pos, uint8_t *pt, size_t pt_size, uint8_t *ct, size_t ct_size, size_t *ct_len);
int crypt_decrypt(psa_key_id_t key, uint8_t *nonce, uint8_t *pos, uint8_t *ct, size_t ct_size, uint8_t *pt, size_t pt_size, size_t *pt_len);
void crypt_nonce_inc(uint8_t *nonce);
