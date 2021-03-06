/*
 * Based on John the Ripper and modified to integrate with aircrack
 *
 * 	John the Ripper copyright and license.
 *
 * John the Ripper password cracker,
 * Copyright (c) 1996-2013 by Solar Designer.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * As a special exception to the GNU General Public License terms,
 * permission is hereby granted to link the code of this program, with or
 * without modification, with any version of the OpenSSL library and/or any
 * version of unRAR, and to distribute such linked combinations.  You must
 * obey the GNU GPL in all respects for all of the code used other than
 * OpenSSL and unRAR.  If you modify this program, you may extend this
 * exception to your version of the program, but you are not obligated to
 * do so.  (In other words, you may release your derived work under pure
 * GNU GPL version 2 or later as published by the FSF.)
 *
 * (This exception from the GNU GPL is not required for the core tree of
 * John the Ripper, but arguably it is required for -jumbo.)
 *
 * 	Relaxed terms for certain components.
 *
 * In addition or alternatively to the license above, many components are
 * available to you under more relaxed terms (most commonly under cut-down
 * BSD license) as specified in the corresponding source files.
 *
 * For more information on John the Ripper licensing please visit:
 *
 * http://www.openwall.com/john/doc/LICENSE.shtml
 *
 * This software is Copyright (c) 2012 Lukas Odzioba <ukasz at openwall dot net>
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * Code is based on Aircrack-ng source
 *
 * SSE2 code enhancement, Jim Fougeron, Jan, 2013.
 *   Also removed oSSL code: HMAC(EVP_sha1(), ....), and coded what it does
 * (which is simple), inline.
 */
#ifndef USE_GCRYPT
#include <openssl/sha.h>
#else
#include "sha1-git.h"
#endif
#include <string.h>
#include <stdint.h>
#ifdef SIMD_CORE
#include "simd-intrinsics.h"
#else
#include "sse-intrinsics.h"
#endif
#include "aircrack-ng.h"
#include "wpapsk.h"
#include "johnswap.h"
#include "memory.h"
#include "aircrack-util/simd_cpuid.h"

//#define XDEBUG

#if defined(__INTEL_COMPILER)
#define SIMD_PARA_SHA1 1
#elif defined(__clang__)
#define SIMD_PARA_SHA1 1
#elif defined(__llvm__)
#define SIMD_PARA_SHA1 1
#elif defined(__GNUC__) && GCC_VERSION < 40504 // 4.5.4
#define SIMD_PARA_SHA1 1
#elif !defined(__AVX__) && defined(__GNUC__) && GCC_VERSION > 40700 // 4.7.0
#define SIMD_PARA_SHA1 1
#else
#define SIMD_PARA_SHA1 1
#endif

#ifdef SIMD_CORE
#ifdef SIMD_COEF_32
#define NBKEYS (SIMD_COEF_32 * SIMD_PARA_SHA1)
#ifdef _OPENMP
#include <omp.h>
#endif
#else
#define NBKEYS 1
#ifdef _OPENMP
#include <omp.h>
#endif
#endif
#else
#ifdef MMX_COEF
#define NBKEYS (MMX_COEF * SHA1_SSE_PARA)
#ifdef _OPENMP
#include <omp.h>
#endif
#else
#define NBKEYS 1
#ifdef _OPENMP
#include <omp.h>
#endif
#endif
#endif

#include "memdbg.h"

#ifndef SIMD_CORE
#undef SIMDSHA1body
#define SIMDSHA1body SSESHA1body
#endif

#define MIN_KEYS_PER_CRYPT 1
#ifdef JOHN_AVX2
#define MAX_KEYS_PER_CRYPT 8
#else
#define MAX_KEYS_PER_CRYPT 4
#endif

char itoa64[64] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
char atoi64[0x100];

wpapsk_password *wpapass[MAX_THREADS] = {0};

unsigned char *pmk[MAX_THREADS] = {NULL};
unsigned char *xsse_hash1[MAX_THREADS] = {NULL};
unsigned char *xsse_crypt1[MAX_THREADS] = {NULL};
unsigned char *xsse_crypt2[MAX_THREADS] = {NULL};

/* for endianity conversion */
#ifdef SIMD_CORE
#define GETPOS(i, index)                                                       \
	((index & (SIMD_COEF_32 - 1)) * 4                                          \
	 + ((i) & (0xffffffff - 3)) * SIMD_COEF_32                                 \
	 + (3 - ((i) &3))                                                          \
	 + (unsigned int) index / SIMD_COEF_32 * SHA_BUF_SIZ * SIMD_COEF_32 * 4)
#else
#define GETPOS(i, index)                                                       \
	((index & (MMX_COEF - 1)) * 4 + ((i) & (0xffffffff - 3)) * MMX_COEF        \
	 + (3 - ((i) &3))                                                          \
	 + (index >> (MMX_COEF >> 1)) * SHA_BUF_SIZ * MMX_COEF * 4)
#endif

void init_ssecore(int threadid)
{
	if (xsse_hash1[threadid] == NULL)
	{
		//	      printf("allocing ssememory[%d]\n", threadid);
		xsse_hash1[threadid] =
			mem_calloc_align(MAX_KEYS_PER_CRYPT, 2048, MEM_ALIGN_SIMD);
		xsse_crypt1[threadid] =
			mem_calloc_align(MAX_KEYS_PER_CRYPT, 2048, MEM_ALIGN_SIMD);
		xsse_crypt2[threadid] =
			mem_calloc_align(MAX_KEYS_PER_CRYPT, 2048, MEM_ALIGN_SIMD);
		pmk[threadid] =
			mem_calloc_align(NBKEYS, sizeof(wpapsk_hash), MEM_ALIGN_SIMD);
		wpapass[threadid] =
			mem_calloc_align(MAX_KEYS_PER_CRYPT, 2048, MEM_ALIGN_SIMD);
	}
}

void free_ssecore(int threadid)
{
	if (xsse_hash1[threadid] != NULL)
	{
		MEM_FREE(xsse_hash1[threadid]);
		MEM_FREE(xsse_crypt1[threadid]);
		MEM_FREE(xsse_crypt2[threadid]);
		MEM_FREE(pmk[threadid]);
		MEM_FREE(wpapass[threadid]);
	}
}

static void set_key(char *key, int index, wpapsk_password *in)
{
	uint8_t length = strlen(key);
	if (length > PLAINTEXT_LENGTH) length = PLAINTEXT_LENGTH;
	in[index].length = length;
	memcpy(in[index].v, key, length + 1);
}

static MAYBE_INLINE void
wpapsk_sse(int threadid, int count, char *salt, wpapsk_password *in)
{
	int t; // thread count
#ifdef XDEBUG
	int prloop = 0;
#endif
	int salt_length = strlen(salt);
	int slen = salt_length + 4;
	int loops = (count + NBKEYS - 1) / NBKEYS;
	char xsalt[32 + 4];

	unsigned char *sse_hash1 = NULL;
	unsigned char *sse_crypt1 = NULL;
	unsigned char *sse_crypt2 = NULL;
	unsigned char essid[32 + 4];

	sse_hash1 = xsse_hash1[threadid];
	sse_crypt1 = xsse_crypt1[threadid];
	sse_crypt2 = xsse_crypt2[threadid];

	sprintf(xsalt, "%s", salt);

	{
		int index;
		for (index = 0; index < cpuinfo.simdsize; ++index)
		{
// set the length of all hash1 SSE buffer to 64+20 * 8 bits. The 64 is for the ipad/opad,
// the 20 is for the length of the SHA1 buffer that also gets into each crypt.
// Works for SSE2i and SSE2
#ifdef SIMD_CORE
			((unsigned int *)
				 sse_hash1)[15 * SIMD_COEF_32 + (index & (SIMD_COEF_32 - 1))
							+ (unsigned int) index / SIMD_COEF_32 * SHA_BUF_SIZ
								  * SIMD_COEF_32] =
				(84 << 3); // all encrypts are 64+20 bytes.
#else
			((unsigned int *)
				 sse_hash1)[15 * MMX_COEF + (index & (MMX_COEF - 1))
							+ (index >> (MMX_COEF >> 1)) * SHA_BUF_SIZ
								  * MMX_COEF] =
				(84 << 3); // all encrypts are 64+20 bytes.
#endif
			sse_hash1[GETPOS(20, index)] = 0x80;
		}
	}

	//	printf("t = %d, nbkeys = %d, loops = %d, essid = %s\n", count, NBKEYS, loops, xsalt);

	memset(essid, 0, 32 + 4);
	memcpy(essid, xsalt, salt_length);

	for (t = 0; t < loops; t++)
	{
		unsigned int i, k, j;
		union {
			unsigned char c[64];
			uint32_t i[16];
		} buffer[NBKEYS];
		union {
			unsigned char c[40];
			uint32_t i[10];
		} outbuf[NBKEYS];
		SHA_CTX ctx_ipad[NBKEYS];
		SHA_CTX ctx_opad[NBKEYS];

		SHA_CTX sha1_ctx;
		unsigned int *i1, *i2, *o1;
		unsigned char *t_sse_crypt1, *t_sse_crypt2, *t_sse_hash1;

		// All pointers get their offset for this thread here. No further offsetting below.
		t_sse_crypt1 = &sse_crypt1[t * NBKEYS * 20];
		t_sse_crypt2 = &sse_crypt2[t * NBKEYS * 20];
		t_sse_hash1 = &sse_hash1[t * NBKEYS * SHA_BUF_SIZ * 4];
		i1 = (unsigned int *) t_sse_crypt1;
		i2 = (unsigned int *) t_sse_crypt2;
		o1 = (unsigned int *) t_sse_hash1;

		for (j = 0; j < NBKEYS; ++j)
		{
			memcpy(
				buffer[j].c, in[t * NBKEYS + j].v, in[t * NBKEYS + j].length);
			memset(&buffer[j].c[in[t * NBKEYS + j].length],
				   0,
				   64 - in[t * NBKEYS + j].length);
			SHA1_Init(&ctx_ipad[j]);
			SHA1_Init(&ctx_opad[j]);

			for (i = 0; i < 16; i++) buffer[j].i[i] ^= 0x36363636;
			SHA1_Update(&ctx_ipad[j], buffer[j].c, 64);

			for (i = 0; i < 16; i++) buffer[j].i[i] ^= 0x6a6a6a6a;
			SHA1_Update(&ctx_opad[j], buffer[j].c, 64);

// we memcopy from flat into MMX_COEF output buffer's (our 'temp' ctx buffer).
// This data will NOT need to be BE swapped (it already IS BE swapped).
#ifdef SIMD_CORE
			i1[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 0 * SIMD_COEF_32] = ctx_ipad[j].h0;
			i1[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 1 * SIMD_COEF_32] = ctx_ipad[j].h1;
			i1[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 2 * SIMD_COEF_32] = ctx_ipad[j].h2;
			i1[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 3 * SIMD_COEF_32] = ctx_ipad[j].h3;
			i1[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 4 * SIMD_COEF_32] = ctx_ipad[j].h4;

			i2[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 0 * SIMD_COEF_32] = ctx_opad[j].h0;
			i2[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 1 * SIMD_COEF_32] = ctx_opad[j].h1;
			i2[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 2 * SIMD_COEF_32] = ctx_opad[j].h2;
			i2[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 3 * SIMD_COEF_32] = ctx_opad[j].h3;
			i2[(j / SIMD_COEF_32) * SIMD_COEF_32 * 5 + (j & (SIMD_COEF_32 - 1))
			   + 4 * SIMD_COEF_32] = ctx_opad[j].h4;
#else
			i1[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))] =
				ctx_ipad[j].h0;
			i1[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + MMX_COEF] = ctx_ipad[j].h1;
			i1[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 1)] = ctx_ipad[j].h2;
			i1[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + MMX_COEF * 3] = ctx_ipad[j].h3;
			i1[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 2)] = ctx_ipad[j].h4;

			i2[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))] =
				ctx_opad[j].h0;
			i2[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + MMX_COEF] = ctx_opad[j].h1;
			i2[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 1)] = ctx_opad[j].h2;
			i2[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + MMX_COEF * 3] = ctx_opad[j].h3;
			i2[(j / MMX_COEF) * MMX_COEF * 5 + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 2)] = ctx_opad[j].h4;
#endif
			essid[slen - 1] = 1;

			//			HMAC(EVP_sha1(), in[j].v, in[j].length, essid, slen, outbuf[j].c, NULL);
			//			memcpy(&buffer[j], &outbuf[j].c, 20);

			// This code does the HMAC(EVP_....) call.  NOTE, we already have essid
			// appended with BE((int)1) so we simply call a single SHA1_Update
			memcpy(&sha1_ctx, &ctx_ipad[j], sizeof(sha1_ctx));
			SHA1_Update(&sha1_ctx, essid, slen);
			SHA1_Final(outbuf[j].c, &sha1_ctx);
			memcpy(&sha1_ctx, &ctx_opad[j], sizeof(sha1_ctx));
			SHA1_Update(&sha1_ctx, outbuf[j].c, SHA_DIGEST_LENGTH);
			SHA1_Final(outbuf[j].c, &sha1_ctx);

// now convert this from flat into MMX_COEF buffers.   (same as the memcpy() commented out in the last line)
// Also, perform the 'first' ^= into the crypt buffer.  NOTE, we are doing that in BE format
// so we will need to 'undo' that in the end.
#ifdef SIMD_CORE
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 0 * SIMD_COEF_32] = outbuf[j].i[0] = sha1_ctx.h0;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 1 * SIMD_COEF_32] = outbuf[j].i[1] = sha1_ctx.h1;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 2 * SIMD_COEF_32] = outbuf[j].i[2] = sha1_ctx.h2;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 3 * SIMD_COEF_32] = outbuf[j].i[3] = sha1_ctx.h3;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 4 * SIMD_COEF_32] = outbuf[j].i[4] = sha1_ctx.h4;
#else
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))] =
				outbuf[j].i[0] = sha1_ctx.h0;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + MMX_COEF] = outbuf[j].i[1] = sha1_ctx.h1;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 1)] = outbuf[j].i[2] = sha1_ctx.h2;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + MMX_COEF * 3] = outbuf[j].i[3] = sha1_ctx.h3;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 2)] = outbuf[j].i[4] = sha1_ctx.h4;
#endif
		}

		for (i = 1; i < 4096; i++)
		{
			SIMDSHA1body((unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_crypt1,
						 SSEi_MIXED_IN | SSEi_RELOAD | SSEi_OUTPUT_AS_INP_FMT);
			SIMDSHA1body((unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_crypt2,
						 SSEi_MIXED_IN | SSEi_RELOAD | SSEi_OUTPUT_AS_INP_FMT);

			for (j = 0; j < NBKEYS; j++)
			{
#ifdef SIMD_CORE
				unsigned *p =
					&((unsigned int *)
						  t_sse_hash1)[(((j / SIMD_COEF_32) * SHA_BUF_SIZ)
										* SIMD_COEF_32)
									   + (j & (SIMD_COEF_32 - 1))];
				for (k = 0; k < 5; k++) outbuf[j].i[k] ^= p[(k * SIMD_COEF_32)];
#else
				unsigned *p = &((
					unsigned int *) t_sse_hash1)[(((j >> 2) * SHA_BUF_SIZ) << 2)
												 + (j & (MMX_COEF - 1))];
				for (k = 0; k < 5; k++)
					outbuf[j].i[k] ^= p[(k << (MMX_COEF >> 1))];
#endif
			}
		}
		essid[slen - 1] = 2;

		for (j = 0; j < NBKEYS; ++j)
		{
			//			HMAC(EVP_sha1(), in[j].v, in[j].length, essid, slen, &outbuf[j].c[20], NULL);
			//			memcpy(&buffer[j], &outbuf[j].c[20], 20);

			// This code does the HMAC(EVP_....) call.  NOTE, we already have essid
			// appended with BE((int)1) so we simply call a single SHA1_Update
			memcpy(&sha1_ctx, &ctx_ipad[j], sizeof(sha1_ctx));
			SHA1_Update(&sha1_ctx, essid, slen);
			SHA1_Final(&outbuf[j].c[20], &sha1_ctx);
			memcpy(&sha1_ctx, &ctx_opad[j], sizeof(sha1_ctx));
			SHA1_Update(&sha1_ctx, &outbuf[j].c[20], 20);
			SHA1_Final(&outbuf[j].c[20], &sha1_ctx);

// now convert this from flat into MMX_COEF buffers.  (same as the memcpy() commented out in the last line)
// Also, perform the 'first' ^= into the crypt buffer.  NOTE, we are doing that in BE format
// so we will need to 'undo' that in the end. (only 3 dwords of the 2nd block outbuf are worked with).
#ifdef SIMD_CORE
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 0 * SIMD_COEF_32] = outbuf[j].i[5] = sha1_ctx.h0;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 1 * SIMD_COEF_32] = outbuf[j].i[6] = sha1_ctx.h1;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 2 * SIMD_COEF_32] = outbuf[j].i[7] = sha1_ctx.h2;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 3 * SIMD_COEF_32] = sha1_ctx.h3;
			o1[(j / SIMD_COEF_32) * SIMD_COEF_32 * SHA_BUF_SIZ
			   + (j & (SIMD_COEF_32 - 1))
			   + 4 * SIMD_COEF_32] = sha1_ctx.h4;
#else
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))] =
				outbuf[j].i[5] = sha1_ctx.h0;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + MMX_COEF] = outbuf[j].i[6] = sha1_ctx.h1;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 1)] = outbuf[j].i[7] = sha1_ctx.h2;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + MMX_COEF * 3] = sha1_ctx.h3;
			o1[(j / MMX_COEF) * MMX_COEF * SHA_BUF_SIZ + (j & (MMX_COEF - 1))
			   + (MMX_COEF << 2)] = sha1_ctx.h4;
#endif
		}
		for (i = 1; i < 4096; i++)
		{
			SIMDSHA1body((unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_crypt1,
						 SSEi_MIXED_IN | SSEi_RELOAD | SSEi_OUTPUT_AS_INP_FMT);
			SIMDSHA1body((unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_hash1,
						 (unsigned int *) t_sse_crypt2,
						 SSEi_MIXED_IN | SSEi_RELOAD | SSEi_OUTPUT_AS_INP_FMT);
			for (j = 0; j < NBKEYS; j++)
			{
#ifdef SIMD_CORE
				unsigned *p =
					&((unsigned int *)
						  t_sse_hash1)[(((j / SIMD_COEF_32) * SHA_BUF_SIZ)
										* SIMD_COEF_32)
									   + (j & (SIMD_COEF_32 - 1))];
				for (k = 5; k < 8; k++)
					outbuf[j].i[k] ^= p[((k - 5) * SIMD_COEF_32)];
#else
				unsigned *p = &((
					unsigned int *) t_sse_hash1)[(((j >> 2) * SHA_BUF_SIZ) << 2)
												 + (j & (MMX_COEF - 1))];
				for (k = 5; k < 8; k++)
					outbuf[j].i[k] ^= p[((k - 5) << (MMX_COEF >> 1))];
#endif
			}
		}

		for (j = 0; j < NBKEYS; ++j)
		{
			//printf("pmk[threadid][%u] = %p\n", j, (void*) (pmk[threadid] + (64*j)));

			memcpy(pmk[threadid] + (sizeof(wpapsk_hash) * j), outbuf[j].c, 32);
			alter_endianity_to_BE((pmk[threadid] + (sizeof(wpapsk_hash) * j)),
								  8);
		}
	}

	return;
}

void init_atoi()
{
	char *pos;

	memset(atoi64, 0x7F, sizeof(atoi64));
	for (pos = itoa64; pos <= &itoa64[63]; pos++)
		atoi64[ARCH_INDEX(*pos)] = pos - itoa64;
}

//#define XDEBUG 1
//#define ODEBUG 1
int init_wpapsk(char (*key)[MAX_THREADS], char *essid, int threadid)
{
#ifdef ODEBUG
	int prloop = 0;
#endif
	int i = 0;
	int count = 0;
	wpapsk_password
		*inbuffer; //table for candidate passwords (pointer to threads copy)

	inbuffer = wpapass[threadid];

	// clear entire output table
	memset(pmk[threadid], 0, (sizeof(wpapsk_hash) * (cpuinfo.simdsize)));

	for (i = 0; i < cpuinfo.simdsize; i++)
	{
		memset(inbuffer[i].v, 0, sizeof(inbuffer[i].v));
		inbuffer[i].length = 0;
	}

	for (i = 0; i < 8; ++i)
	{
		if (key[i][0] != 0)
		{
			set_key(key[i], i, inbuffer);
#ifdef XDEBUG
			printf("key%d (inbuffer) = %s\n", i + 1, inbuffer[i].v);
#endif
			count = i + 1;
		}
	}

#ifdef XDEBUG
//	printf("%d key (%s) (%s) (%s) (%s)\n",threadid, key1,key2,key3,key4);
#endif

	wpapsk_sse(threadid, count, essid, inbuffer);

	return 0;
}
