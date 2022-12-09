/*
 * Part of liblithium, under the Apache License v2.0.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <lithium/gimli_aead.h>

#include "gimli_common.h"
#include "opt.h"

#include <string.h>

static void load_words(uint32_t *s, const unsigned char *p, size_t nw)
{
    size_t i;
    for (i = 0; i < nw; ++i)
    {
        s[i] = gimli_load(&p[4 * i]);
    }
}

void gimli_aead_init(gimli_state *g,
                     const unsigned char n[GIMLI_AEAD_NONCE_LEN],
                     const unsigned char k[GIMLI_AEAD_KEY_LEN])
{
    g->offset = 0;
    load_words(g->state, n, 4);
    load_words(&g->state[4], k, 8);
    gimli(g->state);
}

void gimli_aead_update_ad(gimli_state *g, const unsigned char *ad, size_t adlen)
{
    gimli_absorb(g, ad, adlen);
}

void gimli_aead_final_ad(gimli_state *g)
{
    gimli_pad(g);
    g->offset = GIMLI_RATE - 1;
    gimli_advance(g);
}

static void encrypt_update(gimli_state *g, unsigned char *c,
                           const unsigned char *m, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i)
    {
        gimli_absorb_byte(g, m[i]);
        c[i] = gimli_squeeze_byte(g);
        gimli_advance(g);
    }
}

void gimli_aead_encrypt_update(gimli_state *g, unsigned char *c,
                               const unsigned char *m, size_t len)
{
#if (LITH_SPONGE_WORDS)
    const size_t first_block_len = (GIMLI_RATE - g->offset) % GIMLI_RATE;
    if (len >= GIMLI_RATE + first_block_len)
    {
        encrypt_update(g, c, m, first_block_len);
        c += first_block_len;
        m += first_block_len;
        len -= first_block_len;
        do
        {
#if (LITH_SPONGE_VECTORS)
            *(block *)c = (*(block *)g->state ^= *(const block *)m);
            c += GIMLI_RATE;
            m += GIMLI_RATE;
#else
            size_t i;
            for (i = 0; i < GIMLI_RATE / 4; ++i)
            {
                g->state[i] ^= gimli_load(m);
                gimli_store(c, g->state[i]);
                c += 4;
                m += 4;
            }
#endif
            gimli(g->state);
            len -= GIMLI_RATE;
        } while (len >= GIMLI_RATE);
    }
#endif
    encrypt_update(g, c, m, len);
}

void gimli_aead_encrypt_final(gimli_state *g, unsigned char *t, size_t len)
{
    gimli_pad(g);
    gimli_squeeze(g, t, len);
}

static void decrypt_update(gimli_state *g, unsigned char *m,
                           const unsigned char *c, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i)
    {
        m[i] = c[i] ^ gimli_squeeze_byte(g);
        gimli_absorb_byte(g, m[i]);
        gimli_advance(g);
    }
}

void gimli_aead_decrypt_update(gimli_state *g, unsigned char *m,
                               const unsigned char *c, size_t len)
{
#if (LITH_SPONGE_WORDS)
    const size_t first_block_len = (GIMLI_RATE - g->offset) % GIMLI_RATE;
    if (len >= GIMLI_RATE + first_block_len)
    {
        decrypt_update(g, m, c, first_block_len);
        m += first_block_len;
        c += first_block_len;
        len -= first_block_len;
        do
        {
            /*
             * We absorb the message data back into the gimli_state after
             * outputting it, which amounts to:
             * g->state ^= m;
             * but we can rewrite as:
             * g->state ^= g->state ^ c;
             * and again as:
             * g->state = g->state ^ g->state ^ c;
             * and finally:
             * g->state = c;
             * This is easy to do when operating on words or blocks.
             */
#if (LITH_SPONGE_VECTORS)
            const block cb = *(const block *)c;
            *(block *)m = *(block *)g->state ^ cb;
            *(block *)g->state = cb;
            m += GIMLI_RATE;
            c += GIMLI_RATE;
#else
            size_t i;
            for (i = 0; i < GIMLI_RATE / 4; ++i)
            {
                const uint32_t cw = gimli_load(c);
                gimli_store(m, g->state[i] ^ cw);
                g->state[i] = cw;
                m += 4;
                c += 4;
            }
#endif
            gimli(g->state);
            len -= GIMLI_RATE;
        } while (len >= GIMLI_RATE);
    }
#endif
    decrypt_update(g, m, c, len);
}

bool gimli_aead_decrypt_final(gimli_state *g, const unsigned char *t,
                              size_t tlen)
{
    unsigned char mismatch = 0;
    size_t i;
    gimli_pad(g);
    g->offset = GIMLI_RATE - 1;
    for (i = 0; i < tlen; ++i)
    {
        gimli_advance(g);
        mismatch |= t[i] ^ gimli_squeeze_byte(g);
    }
    return mismatch == 0;
}

void gimli_aead_encrypt(unsigned char *c, unsigned char *t, size_t tlen,
                        const unsigned char *m, size_t len,
                        const unsigned char *ad, size_t adlen,
                        const unsigned char n[GIMLI_AEAD_NONCE_LEN],
                        const unsigned char k[GIMLI_AEAD_KEY_LEN])
{
    gimli_state g;
    gimli_aead_init(&g, n, k);
    gimli_aead_update_ad(&g, ad, adlen);
    gimli_aead_final_ad(&g);
    gimli_aead_encrypt_update(&g, c, m, len);
    gimli_aead_encrypt_final(&g, t, tlen);
}

bool gimli_aead_decrypt(unsigned char *m, const unsigned char *c, size_t len,
                        const unsigned char *t, size_t tlen,
                        const unsigned char *ad, size_t adlen,
                        const unsigned char n[GIMLI_AEAD_NONCE_LEN],
                        const unsigned char k[GIMLI_AEAD_KEY_LEN])
{
    bool success;
    unsigned char mask;
    size_t i;
    gimli_state g;
    gimli_aead_init(&g, n, k);
    gimli_aead_update_ad(&g, ad, adlen);
    gimli_aead_final_ad(&g);
    gimli_aead_decrypt_update(&g, m, c, len);
    success = gimli_aead_decrypt_final(&g, t, tlen);
    mask = (unsigned char)(~(unsigned int)success + 1);
    for (i = 0; i < len; ++i)
    {
        m[i] &= mask;
    }
    return success;
}
