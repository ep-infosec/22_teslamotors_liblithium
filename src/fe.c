/*
 * Part of liblithium, under the Apache License v2.0.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from STROBE's x25519.c, under the MIT license.
 * STROBE is Copyright (c) 2015-2016 Cryptography Research, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "fe.h"

#include <string.h>

#define WLEN (LITH_X25519_WBITS / 8)

static limb read_limb(const unsigned char *p)
{
    return (limb)((limb)p[0] | (limb)p[1] << 8
#if (WLEN >= 4)
                  | (limb)p[2] << 16 | (limb)p[3] << 24
#if (WLEN >= 8)
                  | (limb)p[4] << 32 | (limb)p[5] << 40 | (limb)p[6] << 48 |
                  (limb)p[7] << 56
#endif
#endif
    );
}

static void write_limb(unsigned char *p, limb x)
{
    p[0] = (unsigned char)(x & 0xFFU);
    p[1] = (unsigned char)((x >> 8) & 0xFFU);
#if (WLEN >= 4)
    p[2] = (unsigned char)((x >> 16) & 0xFFU);
    p[3] = (unsigned char)((x >> 24) & 0xFFU);
#if (WLEN >= 8)
    p[4] = (unsigned char)((x >> 32) & 0xFFU);
    p[5] = (unsigned char)((x >> 40) & 0xFFU);
    p[6] = (unsigned char)((x >> 48) & 0xFFU);
    p[7] = (unsigned char)((x >> 56) & 0xFFU);
#endif
#endif
}

void read_limbs(limb x[NLIMBS], const unsigned char *in)
{
    int i;
    for (i = 0; i < NLIMBS; ++i)
    {
        x[i] = read_limb(&in[i * WLEN]);
    }
}

void write_limbs(unsigned char *out, const limb x[NLIMBS])
{
    int i;
    for (i = 0; i < NLIMBS; ++i)
    {
        write_limb(&out[i * WLEN], x[i]);
    }
}

/*
 * Precondition: carry is small.
 * Invariant: result of propagate is < 2^255 + 1 word
 * In particular, always less than 2p.
 * Also, output x >= min(x,19)
 */
static void propagate(fe x, limb carry)
{
    int i;
    carry <<= 1;
    carry |= x[NLIMBS - 1] >> (LITH_X25519_WBITS - 1);
    carry *= 19;
    x[NLIMBS - 1] &= ~LIMB_HIGH_BIT_MASK;
    for (i = 0; i < NLIMBS; ++i)
    {
        x[i] = adc(&carry, x[i], 0);
    }
}

void add(fe out, const fe a, const fe b)
{
    limb carry = 0;
    int i;
    for (i = 0; i < NLIMBS; ++i)
    {
        out[i] = adc(&carry, a[i], b[i]);
    }
    propagate(out, carry);
}

void sub(fe out, const fe a, const fe b)
{
    sdlimb carry = -76;
    int i;
    for (i = 0; i < NLIMBS; ++i)
    {
        carry = carry + a[i] - b[i];
        out[i] = (limb)carry;
        carry = asr(carry, LITH_X25519_WBITS);
    }
    propagate(out, (limb)(carry + 2));
}

static void mul_n(fe out, const fe a, const limb *b, int nb)
{
    limb accum[NLIMBS * 2] = {0};
    limb carry;

    int i, j;
    for (i = 0; i < nb; ++i)
    {
        const limb mand = b[i];
        carry = 0;
        for (j = 0; j < NLIMBS; ++j)
        {
            accum[i + j] = mac(&carry, accum[i + j], mand, a[j]);
        }
        accum[i + NLIMBS] = carry;
    }

    carry = 0;
    for (i = 0; i < NLIMBS; ++i)
    {
        out[i] = mac(&carry, accum[i], 38, accum[i + NLIMBS]);
    }
    propagate(out, carry);
}

void mul(fe out, const fe a, const fe b)
{
    mul_n(out, a, b, NLIMBS);
}

void mul_word(fe out, const fe a, limb b)
{
    mul_n(out, a, &b, 1);
}

void mul1(fe a, const fe b)
{
    mul(a, b, a);
}

void sqr1(fe a)
{
    mul1(a, a);
}

limb canon(fe a)
{
    /*
     * Canonicalize a field element a, reducing it to the least residue which
     * is congruent to it mod 2^255-19. Returns 0 if the residue is nonzero.
     *
     * Precondition: x < 2^255 + 1 word
     */
    sdlimb carry_sub = -19;
    limb carry = 19;
    limb res = 0;
    int i;

    /* First, add 19. */
    for (i = 0; i < NLIMBS; ++i)
    {
        a[i] = adc(&carry, a[i], 0);
    }
    propagate(a, carry);

    /*
     * Here, 19 <= x2 < 2^255
     *
     * This is because we added 19, so before propagate it can't be less than
     * 19. After propagate, it still can't be less than 19, because if
     * propagate does anything it adds 19.
     *
     * We know that the high bit must be clear, because either the input was
     * ~2^255 + one word + 19 (in which case it propagates to at most 2 words)
     * or it was < 2^255.
     *
     * So now, if we subtract 19, we will get back to something in [0,2^255-19).
     */
    for (i = 0; i < NLIMBS; ++i)
    {
        carry_sub += a[i];
        a[i] = (limb)carry_sub;
        res |= a[i];
        carry_sub = asr(carry_sub, LITH_X25519_WBITS);
    }
    return (limb)(((dlimb)res - 1) >> LITH_X25519_WBITS);
}

void inv(fe a)
{
    fe b;
    int i;
    (void)memcpy(b, a, sizeof(fe));
    /* Raise to the p-2 = 0x7f..ffeb */
    for (i = 253; i >= 0; --i)
    {
        sqr1(a);
        if (i >= 8 || ((0xeb >> i) & 1))
        {
            mul1(a, b);
        }
    }
}

/*
 * Portable implementation of an arithmetic shift right on a signed double limb.
 * Used for shifting signed carry values to be added in to the next limb.
 * The portable implementation avoids arithmetic shift right on signed values
 * and casts from unsigned to signed values where the result would be negative,
 * as these operations are implementation-dependent.
 */
sdlimb asr(sdlimb x, int b)
{
#if ((-5 >> 1 != -3) || defined(LITH_FORCE_PORTABLE_ASR))
    const dlimb ux = (dlimb)x;
    const dlimb sign = ux >> ((LITH_X25519_WBITS * 2) - 1); /* sign bit */
    const dlimb sign_mask = ~(sign - 1);
    const dlimb sign_extend = ~(((dlimb)-1) >> b);
    const dlimb pos = ux >> b;
    const dlimb neg = sign_extend | pos;
    return (sdlimb)(pos & ~sign_mask) - (sdlimb)((~neg + 1) & sign_mask);
#else
    return x >> b;
#endif
}

/*
 * Multiply-accumulate with addend.
 */
limb mac(limb *carry, limb a, limb b, limb c)
{
    const dlimb t = (dlimb)b * c + a + *carry;
    *carry = (limb)(t >> LITH_X25519_WBITS);
    return (limb)t;
}

/*
 * Add with carry.
 */
limb adc(limb *carry, limb a, limb b)
{
    const dlimb t = (dlimb)a + b + *carry;
    *carry = (limb)(t >> LITH_X25519_WBITS);
    return (limb)t;
}
