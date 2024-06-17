/*
 * LZ4 - Fast LZ compression algorithm
 * Copyright (C) 2011 - 2016, Yann Collet.
 * BSD 2 - Clause License (http://www.opensource.org/licenses/bsd - license.php)
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *	* Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * You can contact the author at :
 *	- LZ4 homepage : http://www.lz4.org
 *	- LZ4 source repository : https://github.com/lz4/lz4
 *
 *	Changed for kernel usage by:
 *	Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 */

/*-************************************
 *	Dependencies
 **************************************/
#include <linux/lz4.h>
#include "lz4defs.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>
#include "lz4armv8/lz4accel.h"

#include "lz4armv8/lz4accel.h"

/*-*****************************
 *	Decompression functions
 *******************************/

#define DEBUGLOG(l, ...) {}	/* disabled */

#ifndef assert
#define assert(condition) ((void)0)
#endif

#ifndef LZ4_FAST_DEC_LOOP
#if defined(__i386__) || defined(__x86_64__)
#define LZ4_FAST_DEC_LOOP 1
#elif defined(__aarch64__)
     /* On aarch64, we disable this optimization for clang because on certain
      * mobile chipsets and clang, it reduces performance. For more information
      * refer to https://github.com/lz4/lz4/pull/707. */
#define LZ4_FAST_DEC_LOOP 1
#else
#define LZ4_FAST_DEC_LOOP 0
#endif
#endif

#if LZ4_FAST_DEC_LOOP
#define FASTLOOP_SAFE_DISTANCE 64
FORCE_O2_INLINE_GCC_PPC64LE void
LZ4_memcpy_using_offset_base(BYTE * dstPtr, const BYTE * srcPtr, BYTE * dstEnd,
			     const size_t offset)
{
	if (offset < 8) {
		dstPtr[0] = srcPtr[0];

		dstPtr[1] = srcPtr[1];
		dstPtr[2] = srcPtr[2];
		dstPtr[3] = srcPtr[3];
		srcPtr += inc32table[offset];
		memcpy(dstPtr + 4, srcPtr, 4);
		srcPtr -= dec64table[offset];
		dstPtr += 8;
	} else {
		memcpy(dstPtr, srcPtr, 8);
		dstPtr += 8;
		srcPtr += 8;
	}

	LZ4_wildCopy8(dstPtr, srcPtr, dstEnd);
}

/* customized variant of memcpy, which can overwrite up to 32 bytes beyond dstEnd
 * this version copies two times 16 bytes (instead of one time 32 bytes)
 * because it must be compatible with offsets >= 16. */
FORCE_O2_INLINE_GCC_PPC64LE void
LZ4_wildCopy32(void *dstPtr, const void *srcPtr, void *dstEnd)
{
	BYTE *d = (BYTE *) dstPtr;
	const BYTE *s = (const BYTE *)srcPtr;
	BYTE *const e = (BYTE *) dstEnd;

	do {
		memcpy(d, s, 16);
		memcpy(d + 16, s + 16, 16);
		d += 32;
		s += 32;
	} while (d < e);
}

FORCE_O2_INLINE_GCC_PPC64LE void
LZ4_memcpy_using_offset(BYTE *dstPtr, const BYTE *srcPtr, BYTE *dstEnd,
			const size_t offset)
{
	BYTE v[8];
	switch (offset) {

	case 1:
		memset(v, *srcPtr, 8);
		goto copy_loop;
	case 2:
		memcpy(v, srcPtr, 2);
		memcpy(&v[2], srcPtr, 2);
		memcpy(&v[4], &v[0], 4);
		goto copy_loop;
	case 4:
		memcpy(v, srcPtr, 4);
		memcpy(&v[4], srcPtr, 4);
		goto copy_loop;
	default:
		LZ4_memcpy_using_offset_base(dstPtr, srcPtr, dstEnd, offset);
		return;
	}

      copy_loop:
	memcpy(dstPtr, v, 8);
	dstPtr += 8;
	while (dstPtr < dstEnd) {
		memcpy(dstPtr, v, 8);
		dstPtr += 8;
	}
}
#endif

/* variant for decompress_unsafe()
 * does not know end of input
 * presumes input is well formed
 * note : will consume at least one byte */
static size_t read_long_length_no_check(const BYTE **pp)
{
	size_t b, l = 0;
	do {
		b = **pp;
		(*pp)++;
		l += b;
	} while (b == 255);
	DEBUGLOG(6,
		 "read_long_length_no_check: +length=%zu using %zu input bytes",
		 l, l / 255 + 1)
	return l;
}

/* core decoder variant for LZ4_decompress_fast*()
 * for legacy support only : these entry points are deprecated.
 * - Presumes input is correctly formed (no defense vs malformed inputs)
 * - Does not know input size (presume input buffer is "large enough")
 * - Decompress a full block (only)
 * @return : nb of bytes read from input.
 * Note : this variant is not optimized for speed, just for maintenance.
 *        the goal is to remove support of decompress_fast*() variants by v2.0
**/
FORCE_INLINE int LZ4_decompress_unsafe_generic(
	const BYTE *const istart, BYTE *const ostart, int decompressedSize,
	size_t prefixSize,
	const BYTE *const dictStart, /* only if dict==usingExtDict */
	const size_t dictSize /* note: =0 if dictStart==NULL */
)
{
	const BYTE *ip = istart;
	BYTE *op = (BYTE *)ostart;
	BYTE *const oend = ostart + decompressedSize;
	const BYTE *const prefixStart = ostart - prefixSize;

	DEBUGLOG(5, "LZ4_decompress_unsafe_generic");
	if (dictStart == NULL)
		assert(dictSize == 0);

	while (1) {
		/* start new sequence */
		unsigned token = *ip++;

		/* literals */
		{
			size_t ll = token >> ML_BITS;
			if (ll == 15) {
				/* long literal length */
				ll += read_long_length_no_check(&ip);
			}
			if ((size_t)(oend - op) < ll)
				return -1; /* output buffer overflow */
			LZ4_memmove(op, ip,
				    ll); /* support in-place decompression */
			op += ll;
			ip += ll;
			if ((size_t)(oend - op) < MFLIMIT) {
				if (op == oend)
					break; /* end of block */
				DEBUGLOG(
					5,
					"invalid: literals end at distance %zi from end of block",
					oend - op);
				/* incorrect end of block :
                 * last match must start at least MFLIMIT==12 bytes before end of output block */
				return -1;
			}
		}

		/* match */
		{
			size_t ml = token & 15;
			size_t const offset = LZ4_readLE16(ip);
			ip += 2;

			if (ml == 15) {
				/* long literal length */
				ml += read_long_length_no_check(&ip);
			}
			ml += MINMATCH;

			if ((size_t)(oend - op) < ml)
				return -1; /* output buffer overflow */

			{
				const BYTE *match = op - offset;

				/* out of range */
				if (offset >
				    (size_t)(op - prefixStart) + dictSize) {
					DEBUGLOG(6, "offset out of range");
					return -1;
				}

				/* check special case : extDict */
				if (offset > (size_t)(op - prefixStart)) {
					/* extDict scenario */
					const BYTE *const dictEnd =
						dictStart + dictSize;
					const BYTE *extMatch =
						dictEnd -
						(offset -
						 (size_t)(op - prefixStart));
					size_t const extml =
						(size_t)(dictEnd - extMatch);
					if (extml > ml) {
						/* match entirely within extDict */
						LZ4_memmove(op, extMatch, ml);
						op += ml;
						ml = 0;
					} else {
						/* match split between extDict & prefix */
						LZ4_memmove(op, extMatch,
							    extml);
						op += extml;
						ml -= extml;
					}
					match = prefixStart;
				}

				/* match copy - slow variant, supporting overlap copy */
				{
					size_t u;
					for (u = 0; u < ml; u++) {
						op[u] = match[u];
					}
				}
			}
			op += ml;
			if ((size_t)(oend - op) < LASTLITERALS) {
				DEBUGLOG(
					5,
					"invalid: match ends at distance %zi from end of block",
					oend - op);
				/* incorrect end of block :
                 * last match must stop at least LASTLITERALS==5 bytes before end of output block */
				return -1;
			}
		} /* match */
	} /* main loop */
	return (int)(ip - istart);
}

/* Read the variable-length literal or match length.
 *
 * @ip : input pointer
 * @ilimit : position after which if length is not decoded, the input is necessarily corrupted.
 * @initial_check - check ip >= ipmax before start of loop.  Returns initial_error if so.
 * @error (output) - error code.  Must be set to 0 before call.
**/
typedef size_t Rvl_t;
static const Rvl_t rvl_error = (Rvl_t)(-1);
static FORCE_INLINE Rvl_t read_variable_length(const BYTE **ip,
					       const BYTE *ilimit,
					       int initial_check)
{
	Rvl_t s, length = 0;
	assert(ip != NULL);
	assert(*ip != NULL);
	assert(ilimit != NULL);
	if (initial_check &&
	    unlikely((*ip) >= ilimit)) { /* read limit reached */
		return rvl_error;
	}
	do {
		s = **ip;
		(*ip)++;
		length += s;
		if (unlikely((*ip) > ilimit)) { /* read limit reached */
			return rvl_error;
		}
		/* accumulator overflow detection (32-bit mode only) */
		if ((sizeof(length) < 8) &&
		    unlikely(length > ((Rvl_t)(-1) / 2))) {
			return rvl_error;
		}
	} while (s == 255);

	return length;
}

/*
 * LZ4_decompress_generic() :
 * This generic decompression function covers all use cases.
 * It shall be instantiated several times, using different sets of directives.
 * Note that it is important for performance that this function really get inlined,
 * in order to remove useless branches during compilation optimization.
 */
static FORCE_INLINE int
__LZ4_decompress_generic(const char *const src, char *const dst, const BYTE * ip, BYTE * op, int srcSize,
		       /*
		 * If endOnInput == endOnInputSize,
		 * this value is `dstCapacity`
		 */
	 int outputSize,
	 /* endOnOutputSize, endOnInputSize */
	 endCondition_directive endOnInput,
	 /* full, partial */
	 earlyEnd_directive partialDecoding,
	 /* noDict, withPrefix64k */
	 dict_directive dict,
	 /* always <= dst, == dst when no prefix */
	 const BYTE * const lowPrefix,
	 /* only if dict == usingExtDict */
	 const BYTE * const dictStart,
	 /* note : = 0 if noDict */
	 const size_t dictSize
	 )
{
	const BYTE * const iend = src + srcSize;

	BYTE * const oend = dst + outputSize;
	BYTE *cpy;

	const int safeDecode = (endOnInput == endOnInputSize);
	const int checkOffset = ((safeDecode) && (dictSize < (int)(64 * KB)));

	/* Set up the "end" pointers for the shortcut. */
	const BYTE *const shortiend = iend -
	    (endOnInput ? 14 : 8) /*maxLL*/ - 2 /*offset*/;
	const BYTE *const shortoend = oend -
	    (endOnInput ? 14 : 8) /*maxLL*/ - 18 /*maxML*/;

	const BYTE *match;
	size_t offset;
	unsigned int token;
	size_t length;

	DEBUGLOG(5, "%s (srcSize:%i, dstSize:%i)", __func__,
		 srcSize, outputSize);

	/* Special cases */
	assert(lowPrefix <= op);
	assert(src != NULL);

	/* Empty output buffer */
	if ((endOnInput) && (unlikely(outputSize == 0)))
		return ((srcSize == 1) && (*ip == 0)) ? 0 : -1;

	if ((!endOnInput) && (unlikely(outputSize == 0)))
		return (*ip == 0 ? 1 : -1);

	if ((endOnInput) && unlikely(srcSize == 0))
		return -1;

#if LZ4_FAST_DEC_LOOP
	if ((oend - op) < FASTLOOP_SAFE_DISTANCE) {
		DEBUGLOG(6, "skip fast decode loop");
		goto safe_decode;
	}

	{
	    const BYTE * const iend = src + srcSize;

		BYTE * const oend = dst + outputSize;
		BYTE *cpy;

		const BYTE *const dictEnd =
			(dictStart == NULL) ? NULL : dictStart + dictSize;

		const int checkOffset = (dictSize < (int)(64 * KB));

		/* Set up the "end" pointers for the shortcut. */
		const BYTE *const shortiend =
			iend - 14 /*maxLL*/ - 2 /*offset*/;
		const BYTE *const shortoend =
			oend - 14 /*maxLL*/ - 18 /*maxML*/;

		const BYTE *match;
		size_t offset;
		unsigned token;
		size_t length;

		DEBUGLOG(5, "LZ4_decompress_generic (srcSize:%i, dstSize:%i)",
			 srcSize, outputSize);

		/* Special cases */
		assert(lowPrefix <= op);
		if (unlikely(outputSize == 0)) {
			/* Empty output buffer */
			if (partialDecoding)
				return 0;
			return ((srcSize == 1) && (*ip == 0)) ? 0 : -1;
		}
		if (unlikely(srcSize == 0)) {
			return -1;
		}

		/* LZ4_FAST_DEC_LOOP:
     * designed for modern OoO performance cpus,
     * where copying reliably 32-bytes is preferable to an unpredictable branch.
     * note : fast loop may show a regression for some client arm chips. */
#if LZ4_FAST_DEC_LOOP
		if ((oend - op) < FASTLOOP_SAFE_DISTANCE) {
			DEBUGLOG(6, "skip fast decode loop");
			goto safe_decode;
		}

		/* Fast loop : decode sequences as long as output < oend-FASTLOOP_SAFE_DISTANCE */
		while (1) {
			/* Main fastloop assertion: We can always wildcopy FASTLOOP_SAFE_DISTANCE */
			assert(oend - op >= FASTLOOP_SAFE_DISTANCE);
			assert(ip < iend);
		}
		token = *ip++;
		length = token >> ML_BITS;	/* literal length */

		assert(!endOnInput || ip <= iend);	/* ip < iend before the increment */

		/* decode literal length */
		if (length == RUN_MASK) {
			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - RUN_MASK,
						 endOnInput, endOnInput,
						 &error);
			if (error == initial_error) {
				goto _output_error;
			}
			if ((safeDecode)
			    && unlikely((uptrval) (op) + length <
					(uptrval) (op))) {
				goto _output_error;
			}	/* overflow detection */
			if ((safeDecode)
			    && unlikely((uptrval) (ip) + length <
					(uptrval) (ip))) {
				goto _output_error;
			}

			/* overflow detection */
			/* copy literals */
			cpy = op + length;
			LZ4_STATIC_ASSERT(MFLIMIT >= WILDCOPYLENGTH);
			if (endOnInput) {	/* LZ4_decompress_safe() */
				if ((cpy > oend - 32)
				    || (ip + length > iend - 32)) {
					goto safe_literal_copy;
				}
				LZ4_wildCopy32(op, ip, cpy);
			} else {	/* LZ4_decompress_fast() */
				if (cpy > oend - 8) {
					goto safe_literal_copy;
				}
				LZ4_wildCopy8(op, ip, cpy);
				/* LZ4_decompress_fast() cannot copy more than 8 bytes at a time */
				/* it doesn't know input length, and only relies on end-of-block */
				/* properties */
			}
			ip += length;
			op = cpy;
		} else {
			cpy = op + length;
			if (endOnInput) {	/* LZ4_decompress_safe() */
				DEBUGLOG(7,
					 "copy %u bytes in a 16-bytes stripe",
					 (unsigned)length);
				/* We don't need to check oend */
				/* since we check it once for each loop below */
				if (ip > iend - (16 + 1)) {	/*max lit + offset + nextToken */
					goto safe_literal_copy;
				}
				/* Literals can only be 14, but hope compilers optimize */
				/*if we copy by a register size */
				memcpy(op, ip, 16);
			} else {
				/* LZ4_decompress_fast() cannot copy more than 8 bytes at a time */
				/* it doesn't know input length, and relies on end-of-block */
				/* properties */
				memcpy(op, ip, 8);
				if (length > 8) {
					memcpy(op + 8, ip + 8, 8);
				}
			}
			ip += length;
			op = cpy;
		}

		/* get offset */
		offset = LZ4_readLE16(ip);
		ip += 2;	/* end-of-block condition violated */
		match = op - offset;

		/* get matchlength */
		length = token & ML_MASK;

		if ((checkOffset) && (unlikely(match + dictSize < lowPrefix))) {
			goto _output_error;
		}
		/* Error : offset outside buffers */
		if (length == ML_MASK) {
			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - LASTLITERALS + 1,
						 endOnInput, 0, &error);
			if (error != ok) {
				goto _output_error;
			}
			if ((safeDecode)
			    && unlikely((uptrval) (op) + length < (uptrval) op)) {
				goto _output_error;
			}	/* overflow detection */
			length += MINMATCH;
			if (op + length >= oend - FASTLOOP_SAFE_DISTANCE) {
				goto safe_match_copy;
			}
		} else {
			length += MINMATCH;
			if (op + length >= oend - FASTLOOP_SAFE_DISTANCE) {
				goto safe_match_copy;
			}

			/* Fastpath check: Avoids a branch in LZ4_wildCopy32 if true */
			if ((match >= lowPrefix)) {
				if (offset >= 8) {
					memcpy(op, match, 8);
					memcpy(op + 8, match + 8, 8);
					memcpy(op + 16, match + 16, 2);
					op += length;
					continue;
				}
			}
		}

		/* copy match within block */
		cpy = op + length;

		assert((op <= oend) && (oend - op >= 32));
		if (unlikely(offset < 16)) {
			LZ4_memcpy_using_offset(op, match, cpy, offset);
		} else {
			LZ4_wildCopy32(op, match, cpy);
		}

		op = cpy;	/* wildcopy correction */
	}
      safe_decode:
#endif
	/* Main Loop : decode sequences */
	while (1) {
		token = *ip++;
		length = token >> ML_BITS;

		/* ip < iend before the increment */
		assert(!endOnInput || ip <= iend);

		/*
		 * A two-stage shortcut for the most common case:
		 * 1) If the literal length is 0..14, and there is enough
		 * space, enter the shortcut and copy 16 bytes on behalf
		 * of the literals (in the fast mode, only 8 bytes can be
		 * safely copied this way).
		 * 2) Further if the match length is 4..18, copy 18 bytes
		 * in a similar manner; but we ensure that there's enough
		 * space in the output for those 18 bytes earlier, upon
		 * entering the shortcut (in other words, there is a
		 * combined check for both stages).
		 *
		 * The & in the likely() below is intentionally not && so that
		 * some compilers can produce better parallelized runtime code
		 */
		if ((endOnInput ? length != RUN_MASK : length <= 8)
		    /*
		     * strictly "less than" on input, to re-enter
		     * the loop with at least one byte
		     */
		    && likely((endOnInput ? ip < shortiend : 1) &
			      (op <= shortoend))) {
			/* Copy the literals */
			LZ4_memcpy(op, ip, endOnInput ? 16 : 8);
			op += length;
			ip += length;

			/*
			 * The second stage:
			 * prepare for match copying, decode full info.
			 * If it doesn't work out, the info won't be wasted.
			 */
			length = token & ML_MASK;	/* match length */
			offset = LZ4_readLE16(ip);
			ip += 2;
			match = op - offset;
			assert(match <= op);	/* check overflow */

			/* Do not deal with overlapping matches. */
			if ((length != ML_MASK) &&
			    (offset >= 8) &&
			    (dict == withPrefix64k || match >= lowPrefix)) {
				/* Copy the match. */
				LZ4_memcpy(op + 0, match + 0, 8);
				LZ4_memcpy(op + 8, match + 8, 8);
				LZ4_memcpy(op + 16, match + 16, 2);
				op += length + MINMATCH;
				/* Both stages worked, load the next token. */
				continue;
			}

			/*
			 * The second stage didn't work out, but the info
			 * is ready. Propel it right to the point of match
			 * copying.
			 */
			goto _copy_match;
		}

		/* decode literal length */
		if (length == RUN_MASK) {

			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - RUN_MASK,
						 endOnInput, endOnInput,
						 &error);
			if (error == initial_error)
				goto _output_error;

			if ((safeDecode)
			    && unlikely((uptrval) (op) +
					length < (uptrval) (op))) {
				/* overflow detection */
				goto _output_error;
			}
			if ((safeDecode)
			    && unlikely((uptrval) (ip) +
					length < (uptrval) (ip))) {
				/* overflow detection */
				goto _output_error;
			}
		}

		/* copy literals */
		cpy = op + length;
#if LZ4_FAST_DEC_LOOP
	      safe_literal_copy:
#endif
		LZ4_STATIC_ASSERT(MFLIMIT >= WILDCOPYLENGTH);

		if (((endOnInput) && ((cpy > oend - MFLIMIT)
				      || (ip + length >
					  iend - (2 + 1 + LASTLITERALS))))
		    || ((!endOnInput) && (cpy > oend - WILDCOPYLENGTH))) {
			if (partialDecoding) {
				if (cpy > oend) {
					/*
					 * Partial decoding :
					 * stop in the middle of literal segment
					 */
					cpy = oend;
					length = oend - op;
				}
				if ((endOnInput)
				    && (ip + length > iend)) {
					/*
					 * Error :
					 * read attempt beyond
					 * end of input buffer
					 */
					goto _output_error;
				}
			} else {
				if ((!endOnInput)
				    && (cpy != oend)) {
					/*
					 * Error :
					 * block decoding must
					 * stop exactly there
					 */
					goto _output_error;
				}
				if ((endOnInput)
				    && ((ip + length != iend)
					|| (cpy > oend))) {
					/*
					 * Error :
					 * input must be consumed
					 */
					goto _output_error;
				}
			}

			/*
			 * supports overlapping memory regions; only matters
			 * for in-place decompression scenarios
			 */
			LZ4_memmove(op, ip, length);
			ip += length;
			op += length;

			/* Necessarily EOF when !partialDecoding.
			 * When partialDecoding, it is EOF if we've either
			 * filled the output buffer or
			 * can't proceed with reading an offset for following match.
			 */
			if (!partialDecoding || (cpy == oend) || (ip >= (iend - 2)))
				break;
		} else {
			/* may overwrite up to WILDCOPYLENGTH beyond cpy */
			LZ4_wildCopy8(op, ip, cpy);
			ip += length;
			op = cpy;
		}

		/* get offset */
		offset = LZ4_readLE16(ip);
		ip += 2;
		match = op - offset;

		/* get matchlength */
		length = token & ML_MASK;

_copy_match:
		if ((checkOffset) && (unlikely(match + dictSize < lowPrefix))) {
			/* Error : offset outside buffers */
			goto _output_error;
		}

		if (length == ML_MASK) {

			variable_length_error error = ok;
			length +=
			    read_variable_length(&ip, iend - LASTLITERALS + 1,
						 endOnInput, 0, &error);
			if (error != ok)
				goto _output_error;

			if ((safeDecode)
				&& unlikely(
					(uptrval)(op) + length < (uptrval)op)) {
				/* overflow detection */
				goto _output_error;
			}
		}

		length += MINMATCH;

#if LZ4_FAST_DEC_LOOP
safe_match_copy:
#endif

#if 0
		/* match starting within external dictionary */
		if ((dict == usingExtDict) && (match < lowPrefix)) {
			if (unlikely(op + length > oend - LASTLITERALS)) {
				/* doesn't respect parsing restriction */
				if (!partialDecoding)
					goto _output_error;
				length = min(length, (size_t)(oend - op));
			}

			if (length <= (size_t)(lowPrefix - match)) {
				/*
				 * match fits entirely within external
				 * dictionary : just copy
				 */
				memmove(op, dictEnd - (lowPrefix - match),
					length);
				op += length;
			} else {
				/*
				 * match stretches into both external
				 * dictionary and current block
				 */
				size_t const copySize = (size_t)(lowPrefix - match);
				size_t const restSize = length - copySize;

				LZ4_memcpy(op, dictEnd - copySize, copySize);
				op += copySize;
				if (restSize > (size_t)(op - lowPrefix)) {
					/* overlap copy */
					BYTE * const endOfMatch = op + restSize;
					const BYTE *copyFrom = lowPrefix;

					while (op < endOfMatch)
						*op++ = *copyFrom++;
				} else {
					LZ4_memcpy(op, lowPrefix, restSize);
					op += restSize;
				}
			}
			continue;
		}
#endif

		/* copy match within block */
		cpy = op + length;

		/*
		 * partialDecoding :
		 * may not respect endBlock parsing restrictions
		 */
		assert(op <= oend);
		if (partialDecoding &&
		    (cpy > oend - MATCH_SAFEGUARD_DISTANCE)) {
			size_t const mlen = min(length, (size_t)(oend - op));
			const BYTE * const matchEnd = match + mlen;
			BYTE * const copyEnd = op + mlen;

			if (matchEnd > op) {
				/* overlap copy */
				while (op < copyEnd)
					*op++ = *match++;
			} else {
				LZ4_memcpy(op, match, mlen);
			}
			op = copyEnd;
			if (op == oend)
				break;
			continue;
		}

		if (unlikely(offset < 8)) {
			op[0] = match[0];
			op[1] = match[1];
			op[2] = match[2];
			op[3] = match[3];
			match += inc32table[offset];
			LZ4_memcpy(op + 4, match, 4);
			match -= dec64table[offset];
		} else {
			LZ4_copy8(op, match);
			match += 8;
		}

		op += 8;

		if (unlikely(cpy > oend - MATCH_SAFEGUARD_DISTANCE)) {
			BYTE * const oCopyLimit = oend - (WILDCOPYLENGTH - 1);

			if (cpy > oend - LASTLITERALS) {
				/*
				 * Error : last LASTLITERALS bytes
				 * must be literals (uncompressed)
				 */
				goto _output_error;
			}

			if (op < oCopyLimit) {
				LZ4_wildCopy8(op, match, oCopyLimit);
				match += oCopyLimit - op;
				op = oCopyLimit;
			}
			while (op < cpy)
				*op++ = *match++;
		} else {
			LZ4_copy8(op, match);
			if (length > 16)
				LZ4_wildCopy8(op + 8, match + 8, cpy);
		}
		op = cpy; /* wildcopy correction */
	}

	/* end of decoding */
	if (endOnInput) {
		/* Nb of output bytes decoded */
		return (int) (((char *)op) - dst);
	} else {
		/* Nb of input bytes read */
		return (int) (((const char *)ip) - src);
	}

	/* Overflow error detected */
_output_error:
	return (int) (-(((const char *)ip) - src)) - 1;
}

static FORCE_INLINE int LZ4_decompress_generic(
	 const char * const src,
	 char * const dst,
	 int srcSize,
		/*
		 * If endOnInput == endOnInputSize,
		 * this value is `dstCapacity`
		 */
	 int outputSize,
	 /* full, partial */
	 earlyEnd_directive partialDecoding,
	 /* noDict, withPrefix64k, usingExtDict */
	 dict_directive dict,
	 /* always <= dst, == dst when no prefix */
	 const BYTE * const lowPrefix,
	 /* only if dict == usingExtDict */
	 const BYTE * const dictStart,
	 /* note : = 0 if noDict */
	 const size_t dictSize
	 )
{
	return __LZ4_decompress_generic(src, dst, (const BYTE *)src, (BYTE *)dst, srcSize, outputSize, partialDecoding, dict, lowPrefix, dictStart, dictSize);
}

int LZ4_decompress_safe(const char *source, char *dest, int compressedSize,
			int maxDecompressedSize)
{
	return __LZ4_decompress_generic(src, dst, (const BYTE *)src, (BYTE *)dst, srcSize, outputSize, endOnInput, partialDecoding, dict, lowPrefix, dictStart, dictSize);
}

int LZ4_decompress_safe(const char *source, char *dest,
	int compressedSize, int maxDecompressedSize)
{
	return LZ4_decompress_generic(source, dest,
				      compressedSize, maxDecompressedSize,
				      endOnInputSize, decode_full_block,
				      noDict, (BYTE *)dest, NULL, 0);
}

#if IS_ENABLED(CONFIG_EROFS_FS)
int LZ4_decompress_safe_partial(const char *src, char *dst,
	int compressedSize, int targetOutputSize, int dstCapacity)
{
	dstCapacity = min(targetOutputSize, dstCapacity);
	return LZ4_decompress_generic(src, dst, compressedSize, dstCapacity,
				      endOnInputSize, partial_decode,
				      noDict, (BYTE *)dst, NULL, 0);
}
#endif

#if 0
/* ===== Instantiate a few more decoding cases, used more than once. ===== */

int LZ4_decompress_safe_withPrefix64k(const char *source, char *dest,
				      int compressedSize, int maxOutputSize)
{
	return LZ4_decompress_generic(source, dest,
				      compressedSize, maxOutputSize,
				      endOnInputSize, decode_full_block,
				      withPrefix64k,
				      (BYTE *)dest - 64 * KB, NULL, 0);
}

static int LZ4_decompress_safe_withSmallPrefix(const char *source, char *dest,
					       int compressedSize,
					       int maxOutputSize,
					       size_t prefixSize)
{
	return LZ4_decompress_generic(source, dest,
				      compressedSize, maxOutputSize,
				      endOnInputSize, decode_full_block,
				      noDict,
				      (BYTE *)dest - prefixSize, NULL, 0);
}

static int LZ4_decompress_safe_forceExtDict(const char *source, char *dest,
				     int compressedSize, int maxOutputSize,
				     const void *dictStart, size_t dictSize)
{
	return LZ4_decompress_generic(source, dest,
				      compressedSize, maxOutputSize,
				      endOnInputSize, decode_full_block,
				      usingExtDict, (BYTE *)dest,
				      (const BYTE *)dictStart, dictSize);
}

/*
 * The "double dictionary" mode, for use with e.g. ring buffers: the first part
 * of the dictionary is passed as prefix, and the second via dictStart + dictSize.
 * These routines are used only once, in LZ4_decompress_*_continue().
 */
static FORCE_INLINE
int LZ4_decompress_safe_doubleDict(const char *source, char *dest,
				   int compressedSize, int maxOutputSize,
				   size_t prefixSize,
				   const void *dictStart, size_t dictSize)
{
	return LZ4_decompress_generic(source, dest,
				      compressedSize, maxOutputSize,
				      endOnInputSize, decode_full_block,
				      usingExtDict, (BYTE *)dest - prefixSize,
				      (const BYTE *)dictStart, dictSize);
}

static FORCE_INLINE
int LZ4_decompress_fast_doubleDict(const char *source, char *dest,
				   int originalSize, size_t prefixSize,
				   const void *dictStart, size_t dictSize)
{
	return LZ4_decompress_generic(source, dest,
				      0, originalSize,
				      endOnOutputSize, decode_full_block,
				      usingExtDict, (BYTE *)dest - prefixSize,
				      (const BYTE *)dictStart, dictSize);
}

/* ===== streaming decompression functions ===== */

static int LZ4_setStreamDecode(LZ4_streamDecode_t *LZ4_streamDecode,
	const char *dictionary, int dictSize)
{
	LZ4_streamDecode_t_internal *lz4sd =
		&LZ4_streamDecode->internal_donotuse;

	lz4sd->prefixSize = (size_t) dictSize;
	lz4sd->prefixEnd = (const BYTE *) dictionary + dictSize;
	lz4sd->externalDict = NULL;
	lz4sd->extDictSize	= 0;
	return 1;
}

/*
 * *_continue() :
 * These decoding functions allow decompression of multiple blocks
 * in "streaming" mode.
 * Previously decoded blocks must still be available at the memory
 * position where they were decoded.
 * If it's not possible, save the relevant part of
 * decoded data into a safe buffer,
 * and indicate where it stands using LZ4_setStreamDecode()
 */
static int LZ4_decompress_safe_continue(LZ4_streamDecode_t *LZ4_streamDecode,
	const char *source, char *dest, int compressedSize, int maxOutputSize)
{
	LZ4_streamDecode_t_internal *lz4sd =
		&LZ4_streamDecode->internal_donotuse;
	int result;

	if (lz4sd->prefixSize == 0) {
		/* The first call, no dictionary yet. */
		assert(lz4sd->extDictSize == 0);
#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
		result = LZ4_arm64_decompress_safe(source, dest, compressedSize,
						   maxOutputSize, false);
#else
		result = LZ4_decompress_safe(source, dest, compressedSize,
					     maxOutputSize);
#endif
		if (result <= 0)
			return result;
		lz4sd->prefixSize = result;
		lz4sd->prefixEnd = (BYTE *)dest + result;
	} else if (lz4sd->prefixEnd == (BYTE *)dest) {
		/* They're rolling the current segment. */
		if (lz4sd->prefixSize >= 64 * KB - 1)
			result = LZ4_decompress_safe_withPrefix64k(source, dest,
				compressedSize, maxOutputSize);
		else if (lz4sd->extDictSize == 0)
			result = LZ4_decompress_safe_withSmallPrefix(source,
				dest, compressedSize, maxOutputSize,
				lz4sd->prefixSize);
		else
			result = LZ4_decompress_safe_doubleDict(source, dest,
				compressedSize, maxOutputSize,
				lz4sd->prefixSize,
				lz4sd->externalDict, lz4sd->extDictSize);
		if (result <= 0)
			return result;
		lz4sd->prefixSize += result;
		lz4sd->prefixEnd  += result;
	} else {
		/*
		 * The buffer wraps around, or they're
		 * switching to another buffer.
		 */
		lz4sd->extDictSize = lz4sd->prefixSize;
		lz4sd->externalDict = lz4sd->prefixEnd - lz4sd->extDictSize;
		result = LZ4_decompress_safe_forceExtDict(source, dest,
			compressedSize, maxOutputSize,
			lz4sd->externalDict, lz4sd->extDictSize);
		if (result <= 0)
			return result;
		lz4sd->prefixSize = result;
		lz4sd->prefixEnd  = (BYTE *)dest + result;
	}

	return result;
}

ssize_t LZ4_arm64_decompress_safe_partial(const void *source,
			      void *dest,
			      size_t inputSize,
			      size_t outputSize,
			      bool dip)
{
        uint8_t         *dstPtr = dest;
        const uint8_t   *srcPtr = source;
        ssize_t         ret;

#ifdef __ARCH_HAS_LZ4_ACCELERATOR
        /* Go fast if we can, keeping away from the end of buffers */
        if (outputSize > LZ4_FAST_MARGIN && inputSize > LZ4_FAST_MARGIN && lz4_decompress_accel_enable()) {
                ret = lz4_decompress_asm(&dstPtr, dest,
                                         dest + outputSize - LZ4_FAST_MARGIN,
                                         &srcPtr,
                                         source + inputSize - LZ4_FAST_MARGIN,
                                         dip);
                if (ret)
                        return -EIO;
        }
#endif
        /* Finish in safe */
	return __LZ4_decompress_generic(source, dest, srcPtr, dstPtr, inputSize, outputSize, partial_decode, noDict, (BYTE *)dest, NULL, 0);
}

ssize_t LZ4_arm64_decompress_safe(const void *source,
			      void *dest,
			      size_t inputSize,
			      size_t outputSize,
			      bool dip)
{
        uint8_t         *dstPtr = dest;
        const uint8_t   *srcPtr = source;
        ssize_t         ret;

#ifdef __ARCH_HAS_LZ4_ACCELERATOR
        /* Go fast if we can, keeping away from the end of buffers */
        if (outputSize > LZ4_FAST_MARGIN && inputSize > LZ4_FAST_MARGIN && lz4_decompress_accel_enable()) {
                ret = lz4_decompress_asm(&dstPtr, dest,
                                         dest + outputSize - LZ4_FAST_MARGIN,
                                         &srcPtr,
                                         source + inputSize - LZ4_FAST_MARGIN,
                                         dip);
                if (ret)
                        return -EIO;
        }
#endif
        /* Finish in safe */
	return __LZ4_decompress_generic(source, dest, srcPtr, dstPtr, inputSize, outputSize, decode_full_block, noDict, (BYTE *)dest, NULL, 0);
}

#ifndef STATIC
EXPORT_SYMBOL(LZ4_decompress_safe);
EXPORT_SYMBOL(LZ4_decompress_safe_partial);
EXPORT_SYMBOL(LZ4_setStreamDecode);
EXPORT_SYMBOL(LZ4_decompress_safe_continue);
EXPORT_SYMBOL(LZ4_armv8_decompress_safe);
EXPORT_SYMBOL(LZ4_armv8_decompress_safe_partial);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 decompressor");
#endif
