/*
 * Part of liblithium, under the Apache License v2.0.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gimli_common.h"

#include <lithium/gimli.h>
#include <lithium/watchdog.h>

#include "opt.h"

#include <string.h>

uint32_t gimli_load(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

void gimli_store(unsigned char *p, uint32_t x)
{
    p[0] = (unsigned char)(x & 0xFFU);
    p[1] = (unsigned char)((x >> 8) & 0xFFU);
    p[2] = (unsigned char)((x >> 16) & 0xFFU);
    p[3] = (unsigned char)((x >> 24) & 0xFFU);
}

#if (LITH_LITTLE_ENDIAN)
#define OFFSET_SWAP 0U
#elif (LITH_BIG_ENDIAN)
#define OFFSET_SWAP 3U
#endif

void gimli_absorb_byte(gimli_state *g, unsigned char x)
{
#if defined(__TMS320C2000__)
    __byte((int *)g->state, g->offset) ^= x;
#elif ((LITH_LITTLE_ENDIAN || LITH_BIG_ENDIAN) && (CHAR_BIT == 8))
    ((unsigned char *)g->state)[g->offset ^ OFFSET_SWAP] ^= x;
#else
    g->state[g->offset / 4] ^= (uint32_t)x << ((g->offset % 4) * 8);
#endif
}

unsigned char gimli_squeeze_byte(const gimli_state *g)
{
#if defined(__TMS320C2000__)
    return (unsigned char)__byte((int *)g->state, g->offset);
#elif ((LITH_LITTLE_ENDIAN || LITH_BIG_ENDIAN) && (CHAR_BIT == 8))
    return ((const unsigned char *)g->state)[g->offset ^ OFFSET_SWAP];
#else
    return (unsigned char)((g->state[g->offset / 4] >> ((g->offset % 4) * 8)) &
                           0xFFU);
#endif
}

void gimli_advance(gimli_state *g)
{
    ++g->offset;
    if (g->offset == GIMLI_RATE)
    {
#if (LITH_ENABLE_WATCHDOG)
        lith_watchdog_pet();
#endif
        gimli(g->state);
        g->offset = 0;
    }
}

static void absorb(gimli_state *g, const unsigned char *m, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i)
    {
        gimli_absorb_byte(g, m[i]);
        gimli_advance(g);
    }
}

void gimli_absorb(gimli_state *g, const unsigned char *m, size_t len)
{
#if (LITH_SPONGE_WORDS)
    const unsigned first_block_len = (GIMLI_RATE - g->offset) % GIMLI_RATE;
    if (len >= GIMLI_RATE + first_block_len)
    {
        absorb(g, m, first_block_len);
        m += first_block_len;
        len -= first_block_len;
        do
        {
#if (LITH_SPONGE_VECTORS)
            *(block *)g->state ^= *(const block *)m;
            m += GIMLI_RATE;
#else
            size_t i;
            for (i = 0; i < GIMLI_RATE / 4; ++i)
            {
                g->state[i] ^= gimli_load(m);
                m += 4;
            }
#endif
            gimli(g->state);
            len -= GIMLI_RATE;
        } while (len >= GIMLI_RATE);
    }
#endif
    absorb(g, m, len);
}

void gimli_squeeze(gimli_state *g, unsigned char *h, size_t len)
{
    size_t i;
    g->offset = GIMLI_RATE - 1;
    for (i = 0; i < len; ++i)
    {
        gimli_advance(g);
        h[i] = gimli_squeeze_byte(g);
    }
}

void gimli_pad(gimli_state *g)
{
    gimli_absorb_byte(g, 0x01);
    g->state[GIMLI_WORDS - 1] ^= UINT32_C(0x01000000);
}
