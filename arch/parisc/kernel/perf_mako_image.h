/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PERF_MAKO_IMAGE_H
#define PERF_MAKO_IMAGE_H

/*
 * PA-8800/PA-8900 (Mako) image: the RDRs to write, in order, each with its
 * chain and data. data[0] is the most significant doubleword and is shifted
 * in last. This is the first built-in image of the C8000 PDC 2.13 firmware
 * (0x29c920 in the PDC0213 image), which the firmware's PDC_INTRIGUE calls
 * also load. RDR15 on chain 1 holds the counters.
 */
struct perf_mako_rdr {
	uint8_t		rdr;
	uint8_t		chain;
	uint8_t		num_words;
	uint16_t	width;
	const uint64_t	*data;
	const uint64_t	*mask;
};

#define PERF_MAKO_RDR(r, c, d) \
	{ .rdr = (r), .chain = (c), .num_words = ARRAY_SIZE(d), .data = (d) }
#define PERF_MAKO_RDR_MASK(r, c, w, d, m) \
	{ .rdr = (r), .chain = (c), .num_words = ARRAY_SIZE(d), .width = (w), \
	  .data = (d), .mask = (m) }

static const uint64_t perf_mako_rdr0_0[] = {
	0x4001800000000000ULL,
};
static const uint64_t perf_mako_rdr1_0[] = {
	0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr4_0[] = {
	0x0000001000001fffULL, 0x0010007ffc000001ULL, 0xffffffffffffffffULL,
	0xffffffff0007ffffULL, 0xffd607ffff846007ULL, 0xffffa00000000000ULL,
};
static const uint64_t perf_mako_rdr5_0[] = {
	0xc0ffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
	0xffffffffffff0000ULL,
};
static const uint64_t perf_mako_rdr17_0[] = {
	0xc000000000000000ULL,
};
static const uint64_t perf_mako_rdr18_0[] = {
	0xc000000000000000ULL,
};
static const uint64_t perf_mako_rdr20_0[] = {
	0x00ffffffff3fffffULL, 0xffffffffffcfffffULL, 0xfff7fbfc00000000ULL,
};
static const uint64_t perf_mako_rdr21_0[] = {
	0x00ffffffff3fffffULL, 0xffffffffffcfffffULL, 0xfff7fbfc00000000ULL,
};
static const uint64_t perf_mako_rdr22_0[] = {
	0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
	0xfffc000000000000ULL,
};
static const uint64_t perf_mako_rdr23_0[] = {
	0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL,
	0xfffc000000000000ULL,
};
static const uint64_t perf_mako_rdr24_0[] = {
	0xfffffffffffff9ffULL, 0xfe00000000000000ULL,
};
static const uint64_t perf_mako_rdr25_0[] = {
	0xfffffffffffff9ffULL, 0xfe00000000000000ULL,
};
static const uint64_t perf_mako_rdr0_1[] = {
	0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr1_1[] = {
	0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr5_1[] = {
	0x65ffffffffffffffULL, 0xf500000000000000ULL, 0xffffffffffffffffULL,
	0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr6_1[] = {
	0xfff6fff6ffff0c20ULL, 0x000000000000c200ULL, 0x00000000000ee000ULL,
	0x0000000000ee0000ULL, 0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr9_1[] = {
	0x0000000000000001ULL,
};
static const uint64_t perf_mako_rdr15_1[] = {
	0x7fffffec00000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
	0x0000000000000000ULL,
};
/*
 * RDR16_1 selects what the counters count. The firmware's image enables
 * only ctr0 and ctr1 of the first core. Bytes are numbered from the most
 * significant byte of the first doubleword; changed from the firmware:
 *
 * - Byte 14 (event selects, bits 7..4 for the first core's ctr0..ctr3,
 *   bits 3..0 for the second core's) = 0xaa: ctr0 counts the TLB miss
 *   event, ctr2 the instructions retired event.
 * - Byte 58 bits 1:0 = 3: count on the first core's ctr1.
 * - Byte 59 = 0xfa: bits 7 and 5 enable the first core's ctr2 and ctr3,
 *   bits 3 and 1 the second core's ctr0 and ctr1.
 * - Byte 60 = 0xa0: bits 7 and 5 enable the second core's ctr2 and ctr3.
 *
 * Counters without an event selected count cycles, so on every CPU ctr0
 * counts TLB misses, ctr1 and ctr3 cycles and ctr2 instructions retired.
 */
static const uint64_t perf_mako_rdr16_1[] = {
	0x00020200a0000000ULL, 0x000aa4444110aa00ULL, 0x000000000009000fULL,
	0xf000000000000000ULL, 0x0000000005a12000ULL, 0x0000000000000000ULL,
	0x0000000500010555ULL, 0x100003faa0000000ULL, 0x00000ffff0fff000ULL,
	0x0000000000000000ULL, 0x0005000f2f0f0f0fULL, 0x0f0f0f0f0f0f0f30ULL,
	0xfaf0f08000000000ULL, 0x0000000000000000ULL, 0x900ff00000000000ULL,
	0x000000000005a000ULL, 0x0000012000000000ULL, 0x0000000000000005ULL,
	0x0015550400000100ULL, 0x00000000000fffffULL, 0xf0f0000000000000ULL,
	0x0000050000000f0fULL, 0x0f2f0f0f0f0f0f0fULL, 0x0f0f30f0faf00800ULL,
	0x0000000000000000ULL, 0x0000000090000ff0ULL, 0x0000005000050000ULL,
	0x0000000000000000ULL, 0x0020000200000000ULL, 0x0500001000000000ULL,
	0x0800040401000055ULL, 0x500c0000ff000000ULL, 0x0000ff0000aa00aaULL,
	0x0000000000000000ULL, 0x00000000005af000ULL, 0x0b00555000040101ULL,
	0x000000000000ffffULL, 0xfffff00000000000ULL, 0x0000000000000fffULL,
	0xffffffffffffffffULL, 0xfffffffffffffff0ULL, 0x0000000000000000ULL,
	0x0000000fffffffffULL, 0x0000000000000000ULL, 0x00000000ffffffffULL,
	0xffffffffffffffffULL, 0xffffffffff000000ULL, 0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr20_1[] = {
	0xfffffffffffff800ULL, 0x00000000003fffffULL, 0xfffffffe00000000ULL,
	0x00000fffffffffffULL, 0xff80000000000003ULL, 0xffffffffffffe000ULL,
	0x0000000000000000ULL,
};
static const uint64_t perf_mako_rdr24_1[] = {
	0x0000000000000000ULL, 0x0000000000000000ULL,
};

/*
 * The firmware does not store the image verbatim. It merges each RDR with
 * the current contents, new = (image & ~mask) | (current & mask), using
 * the mask table at 0x29ba90 in the PDC0213 image. A set mask bit keeps the
 * current bit. RDR0, RDR1, RDR9 and RDR24 on chain 1 hold live state, and
 * overwriting them hangs the machine or takes an HPMC. The masks below are
 * the firmware's, clipped to the width of each RDR, and are given only for
 * the RDRs where they keep a bit that exists. Those entries also give the
 * width of the RDR in bits, which the merge needs to align what it reads.
 */
static const uint64_t perf_mako_mask0_0[] = {
	0x0200000000000000ULL,
};
static const uint64_t perf_mako_mask1_0[] = {
	0xffc0ffffffffffffULL,
};
static const uint64_t perf_mako_mask20_0[] = {
	0xff00000000c00000ULL, 0x0000000000300000ULL, 0x0008040000000000ULL,
};
static const uint64_t perf_mako_mask21_0[] = {
	0xff00000000c00000ULL, 0x0000000000300000ULL, 0x0008040000000000ULL,
};
static const uint64_t perf_mako_mask22_0[] = {
	0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
	0x0003c00000000000ULL,
};
static const uint64_t perf_mako_mask23_0[] = {
	0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
	0x0003c00000000000ULL,
};
static const uint64_t perf_mako_mask0_1[] = {
	0xfffffbfffffffffeULL,
};
static const uint64_t perf_mako_mask1_1[] = {
	0xffffffcfffffffffULL,
};
static const uint64_t perf_mako_mask9_1[] = {
	0xfffffefffffffffcULL,
};
static const uint64_t perf_mako_mask24_1[] = {
	0xffffffffc3ffffffULL, 0xffffffffffffffffULL,
};

static const struct perf_mako_rdr perf_mako_image[] = {
	PERF_MAKO_RDR_MASK(0, 0, 19, perf_mako_rdr0_0,
			   perf_mako_mask0_0),
	PERF_MAKO_RDR_MASK(1, 0, 64, perf_mako_rdr1_0,
			   perf_mako_mask1_0),
	PERF_MAKO_RDR(4, 0, perf_mako_rdr4_0),
	PERF_MAKO_RDR(5, 0, perf_mako_rdr5_0),
	PERF_MAKO_RDR(17, 0, perf_mako_rdr17_0),
	PERF_MAKO_RDR(18, 0, perf_mako_rdr18_0),
	PERF_MAKO_RDR_MASK(20, 0, 158, perf_mako_rdr20_0,
			   perf_mako_mask20_0),
	PERF_MAKO_RDR_MASK(21, 0, 158, perf_mako_rdr21_0,
			   perf_mako_mask21_0),
	PERF_MAKO_RDR_MASK(22, 0, 210, perf_mako_rdr22_0,
			   perf_mako_mask22_0),
	PERF_MAKO_RDR_MASK(23, 0, 210, perf_mako_rdr23_0,
			   perf_mako_mask23_0),
	PERF_MAKO_RDR(24, 0, perf_mako_rdr24_0),
	PERF_MAKO_RDR(25, 0, perf_mako_rdr25_0),
	PERF_MAKO_RDR_MASK(0, 1, 64, perf_mako_rdr0_1,
			   perf_mako_mask0_1),
	PERF_MAKO_RDR_MASK(1, 1, 64, perf_mako_rdr1_1,
			   perf_mako_mask1_1),
	PERF_MAKO_RDR(5, 1, perf_mako_rdr5_1),
	PERF_MAKO_RDR(6, 1, perf_mako_rdr6_1),
	PERF_MAKO_RDR_MASK(9, 1, 64, perf_mako_rdr9_1,
			   perf_mako_mask9_1),
	PERF_MAKO_RDR(15, 1, perf_mako_rdr15_1),
	PERF_MAKO_RDR(16, 1, perf_mako_rdr16_1),
	PERF_MAKO_RDR(20, 1, perf_mako_rdr20_1),
	PERF_MAKO_RDR_MASK(24, 1, 128, perf_mako_rdr24_1,
			   perf_mako_mask24_1),
};

#endif
