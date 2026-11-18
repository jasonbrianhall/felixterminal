/* aes_dos.c: AES decryption (FIPS-197) for kfn.cpp's encrypted .kfn files,
   with OpenSSL's AES_set_decrypt_key/AES_decrypt interface (see
   include/openssl/aes.h). Byte-oriented and small; speed doesn't matter
   here (a song is a few MB, decrypted once). */
#include <string.h>
#include "openssl/aes.h"

static unsigned char sbox[256], inv_sbox[256];

static unsigned char xtime(unsigned char x) { return (unsigned char)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static unsigned char gmul(unsigned char a, unsigned char b)
{
    unsigned char p = 0;
    while (b) { if (b & 1) p ^= a; a = xtime(a); b >>= 1; }
    return p;
}

static void make_tables(void)
{
    if (sbox[0]) return;                       /* (sbox[0] is 0x63 once made) */
    unsigned char p = 1, q = 1;
    do {                                       /* p walks GF(2^8)* by 3, q by 1/3 */
        p = p ^ xtime(p);
        q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
        unsigned char x = q ^ (unsigned char)((q << 1) | (q >> 7)) ^ (unsigned char)((q << 2) | (q >> 6))
                            ^ (unsigned char)((q << 3) | (q >> 5)) ^ (unsigned char)((q << 4) | (q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (unsigned char)i;
}

int AES_set_decrypt_key(const unsigned char *userKey, const int bits, AES_KEY *key)
{
    if (!userKey || !key) return -1;
    if (bits != 128 && bits != 192 && bits != 256) return -2;
    make_tables();
    int nk = bits / 32, nr = nk + 6, total = 4 * (nr + 1);
    unsigned char w[240];
    memcpy(w, userKey, (size_t)nk * 4);
    unsigned char rcon = 1;
    for (int i = nk; i < total; i++) {
        unsigned char t[4];
        memcpy(t, w + (i - 1) * 4, 4);
        if (i % nk == 0) {
            unsigned char u = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) w[i * 4 + j] = w[(i - nk) * 4 + j] ^ t[j];
    }
    memcpy(key->rd_key, w, (size_t)total * 4);
    key->rounds = nr;
    return 0;
}

void AES_decrypt(const unsigned char *in, unsigned char *out, const AES_KEY *key)
{
    const unsigned char *rk = (const unsigned char *)key->rd_key;
    int nr = key->rounds;
    unsigned char s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[nr * 16 + i];
    for (int round = nr - 1; round >= 0; round--) {
        /* InvShiftRows + InvSubBytes (state is column-major: s[c*4+r]) */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[((c + r) % 4) * 4 + r] = inv_sbox[s[c * 4 + r]];
        for (int i = 0; i < 16; i++) t[i] ^= rk[round * 16 + i];
        if (round > 0) {
            for (int c = 0; c < 4; c++) {          /* InvMixColumns */
                unsigned char *a = t + c * 4;
                unsigned char a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                s[c * 4 + 0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                s[c * 4 + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                s[c * 4 + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                s[c * 4 + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        } else {
            memcpy(s, t, 16);
        }
    }
    memcpy(out, s, 16);
}
