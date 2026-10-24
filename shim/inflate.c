#include "inflate.h"

#define MAXBITS		15
#define MAXLCODES	286
#define MAXDCODES	30
#define FIXLCODES	288

struct huffman {
	short count[MAXBITS + 1];	/* number of codes of each length */
	short symbol[FIXLCODES];	/* symbols ordered by code */
};

static struct {
	const uint8_t *in;
	size_t inlen, incnt;
	uint32_t bitbuf;
	int bitcnt;
	uint8_t *out;
	size_t outcap, outcnt;
	int err;
} s;

static struct huffman lencode, distcode;
static short lengths[MAXLCODES + MAXDCODES];

static uint32_t bits(int need)
{
	uint32_t val = s.bitbuf;

	while (s.bitcnt < need) {
		if (s.incnt == s.inlen) {
			s.err = 1;
			return 0;
		}
		val |= (uint32_t)s.in[s.incnt++] << s.bitcnt;
		s.bitcnt += 8;
	}
	s.bitbuf = val >> need;
	s.bitcnt -= need;
	return val & ((1u << need) - 1);
}

static int stored(void)
{
	s.bitbuf = 0;
	s.bitcnt = 0;
	if (s.incnt + 4 > s.inlen)
		return 2;
	uint32_t len = s.in[s.incnt] | (uint32_t)s.in[s.incnt + 1] << 8;
	uint32_t nlen = s.in[s.incnt + 2] | (uint32_t)s.in[s.incnt + 3] << 8;
	if (len != (~nlen & 0xffff))
		return -2;
	s.incnt += 4;
	if (s.incnt + len > s.inlen)
		return 2;
	if (s.outcnt + len > s.outcap)
		return 1;
	while (len--)
		s.out[s.outcnt++] = s.in[s.incnt++];
	return 0;
}

static int decode(const struct huffman *h)
{
	int len, code = 0, first = 0, index = 0;

	for (len = 1; len <= MAXBITS; len++) {
		code |= bits(1);
		if (s.err)
			return 2;
		int count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -10;
}

/* Returns 0 for a complete code, >0 for an incomplete one, <0 for an over-subscribed one. */
static int construct(struct huffman *h, const short *length, int n)
{
	int symbol, len, left;
	short offs[MAXBITS + 1];

	for (len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (symbol = 0; symbol < n; symbol++)
		h->count[length[symbol]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0)
			h->symbol[offs[length[symbol]]++] = symbol;
	return left;
}

static int codes(const struct huffman *lc, const struct huffman *dc)
{
	static const short lens[29] = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
		35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const short lext[29] = {
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
		3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const short dists[30] = {
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
		257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
		8193, 12289, 16385, 24577 };
	static const short dext[30] = {
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
		7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	int symbol;

	do {
		symbol = decode(lc);
		if (symbol < 0)
			return symbol;
		if (symbol == 2 && s.err)
			return 2;
		if (symbol < 256) {
			if (s.outcnt == s.outcap)
				return 1;
			s.out[s.outcnt++] = symbol;
		} else if (symbol > 256) {
			symbol -= 257;
			if (symbol >= 29)
				return -10;
			size_t len = lens[symbol] + bits(lext[symbol]);
			symbol = decode(dc);
			if (symbol < 0)
				return symbol;
			if (symbol >= 30)
				return -10;
			size_t dist = dists[symbol] + bits(dext[symbol]);
			if (s.err)
				return 2;
			if (dist > s.outcnt)
				return -11;
			if (s.outcnt + len > s.outcap)
				return 1;
			uint8_t *d = s.out + s.outcnt;
			const uint8_t *src = d - dist;
			s.outcnt += len;
			while (len--)
				*d++ = *src++;
			symbol = 0;	/* anything but end-of-block */
		}
	} while (symbol != 256);
	return 0;
}

static int fixed(void)
{
	static int virgin = 1;

	if (virgin) {
		int symbol;

		for (symbol = 0; symbol < 144; symbol++)
			lengths[symbol] = 8;
		for (; symbol < 256; symbol++)
			lengths[symbol] = 9;
		for (; symbol < 280; symbol++)
			lengths[symbol] = 7;
		for (; symbol < FIXLCODES; symbol++)
			lengths[symbol] = 8;
		construct(&lencode, lengths, FIXLCODES);
		for (symbol = 0; symbol < MAXDCODES; symbol++)
			lengths[symbol] = 5;
		construct(&distcode, lengths, MAXDCODES);
		virgin = 0;
	}
	return codes(&lencode, &distcode);
}

static int dynamic(void)
{
	static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	int nlen, ndist, ncode, index, err;

	nlen = bits(5) + 257;
	ndist = bits(5) + 1;
	ncode = bits(4) + 4;
	if (s.err)
		return 2;
	if (nlen > MAXLCODES || ndist > MAXDCODES)
		return -3;
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = bits(3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	err = construct(&lencode, lengths, 19);
	if (err != 0)
		return -4;
	index = 0;
	while (index < nlen + ndist) {
		int symbol = decode(&lencode), len;

		if (symbol < 0)
			return symbol;
		if (s.err)
			return 2;
		if (symbol < 16) {
			lengths[index++] = symbol;
		} else {
			len = 0;
			if (symbol == 16) {
				if (index == 0)
					return -5;
				len = lengths[index - 1];
				symbol = 3 + bits(2);
			} else if (symbol == 17) {
				symbol = 3 + bits(3);
			} else {
				symbol = 11 + bits(7);
			}
			if (index + symbol > nlen + ndist)
				return -6;
			while (symbol--)
				lengths[index++] = len;
		}
	}
	if (lengths[256] == 0)
		return -9;
	err = construct(&lencode, lengths, nlen);
	if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
		return -7;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
		return -8;
	return codes(&lencode, &distcode);
}

/**
 * Raw DEFLATE (RFC 1951) decoder. *out_len is the number of bytes written. Never writes beyond out_cap.
 * @return 0: success, 1: buffer too small, 2: recieved corrupt file
 */
int inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
	int last, type, err = 0;

	s.in = in;
	s.inlen = in_len;
	s.incnt = 0;
	s.bitbuf = 0;
	s.bitcnt = 0;
	s.out = out;
	s.outcap = out_cap;
	s.outcnt = 0;
	s.err = 0;
	do {
		last = bits(1);
		type = bits(2);
		if (s.err) {
			err = 2;
			break;
		}
		err = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : -1;
		if (err)
			break;
	} while (!last);
	*out_len = s.outcnt;
	return err;
}
