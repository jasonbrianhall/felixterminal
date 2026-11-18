/* openssl/aes.h for Felix Chirp on MS-DOS: kfn.cpp decrypts encrypted .kfn
   karaoke files with OpenSSL's AES_set_decrypt_key/AES_decrypt (AES-128,
   ECB, a block at a time). aes_dos.c has just that, so CHIRP.EXE needs no
   OpenSSL. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#define AES_BLOCK_SIZE 16
typedef struct aes_key_st {
    unsigned int rd_key[60];
    int rounds;
} AES_KEY;
int  AES_set_decrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key);
void AES_decrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key);
#ifdef __cplusplus
}
#endif
