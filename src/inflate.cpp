/**
 * @file inflate.cpp
 * @brief Minimal pure-C++ DEFLATE decoder.
 *
 * Implements RFC 1951 — three block types: stored (00), fixed
 * Huffman (01), dynamic Huffman (10). Pattern follows Mark Adler's
 * "puff" reference implementation but adapted for `std::vector`
 * output.
 */

#include "inflate.h"

#include <cstring>

namespace wbsh {

constexpr int MAXBITS  = 15;     // max code length
constexpr int MAXLCODES = 286;   // max literal/length codes
constexpr int MAXDCODES = 30;    // max distance codes
constexpr int MAXCODES = MAXLCODES + MAXDCODES;
constexpr int FIXLCODES = 288;

struct InflateState {
	const std::uint8_t* in;
	std::size_t in_len;
	std::size_t in_pos = 0;
	int bitbuf = 0;
	int bitcnt = 0;
	std::vector<std::uint8_t>& out;
	bool err = false;

	InflateState(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& o)
	    : in(p), in_len(n), out(o) {}

	int bits(int need) {
		long val = bitbuf;
		while (bitcnt < need) {
			if (in_pos >= in_len) { err = true; return 0; }
			val |= (long)in[in_pos++] << bitcnt;
			bitcnt += 8;
		}
		bitbuf = (int)(val >> need);
		bitcnt -= need;
		return (int)val & ((1 << need) - 1);
	}
};

struct InflateHuffman {
	short count[MAXBITS + 1] {};
	short symbol[MAXCODES] {};
};

static int decode(InflateState& s, const InflateHuffman& h) {
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= MAXBITS; ++len) {
		code |= s.bits(1);
		int count = h.count[len];
		if (code - count < first) return h.symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s.err = true;
	return -1;
}

static int construct(InflateHuffman& h, const short* length, int n) {
	short offs[MAXBITS + 1];
	for (int len = 0; len <= MAXBITS; ++len) h.count[len] = 0;
	for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
	if (h.count[0] == n) return 0;
	int left = 1;
	for (int len = 1; len <= MAXBITS; ++len) {
		left <<= 1;
		left -= h.count[len];
		if (left < 0) return left;
	}
	offs[1] = 0;
	for (int len = 1; len < MAXBITS; ++len) offs[len + 1] = offs[len] + h.count[len];
	for (int sym = 0; sym < n; ++sym) {
		if (length[sym] != 0) h.symbol[offs[length[sym]]++] = (short)sym;
	}
	return left;
}

static int codes(InflateState& s, const InflateHuffman& lencode, const InflateHuffman& distcode) {
	static const short lens[29] = {
		3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
	static const short lext[29] = {
		0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
	static const short dists[30] = {
		1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
		1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
	static const short dext[30] = {
		0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
	while (true) {
		int sym = decode(s, lencode);
		if (s.err) return -1;
		if (sym < 256) {
			s.out.push_back((std::uint8_t)sym);
		} else if (sym == 256) {
			break;
		} else {
			sym -= 257;
			if (sym >= 29) return -1;
			int len = lens[sym] + s.bits(lext[sym]);
			sym = decode(s, distcode);
			if (s.err || sym >= 30) return -1;
			int dist = dists[sym] + s.bits(dext[sym]);
			if ((std::size_t)dist > s.out.size()) return -1;
			std::size_t base = s.out.size() - dist;
			for (int i = 0; i < len; ++i) {
				s.out.push_back(s.out[base + i]);
			}
		}
	}
	return 0;
}

static int stored(InflateState& s) {
	s.bitbuf = 0;
	s.bitcnt = 0;
	if (s.in_pos + 4 > s.in_len) return -1;
	int len  = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
	int nlen = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
	s.in_pos += 4;
	if ((len & 0xFFFF) != ((~nlen) & 0xFFFF)) return -1;
	if (s.in_pos + len > s.in_len) return -1;
	s.out.insert(s.out.end(), s.in + s.in_pos, s.in + s.in_pos + len);
	s.in_pos += len;
	return 0;
}

static int fixedBlock(InflateState& s) {
	static InflateHuffman lencode, distcode;
	static bool built = false;
	if (!built) {
		short lens[FIXLCODES];
		int sym;
		for (sym = 0;   sym < 144; ++sym) lens[sym] = 8;
		for (;          sym < 256; ++sym) lens[sym] = 9;
		for (;          sym < 280; ++sym) lens[sym] = 7;
		for (;          sym < FIXLCODES; ++sym) lens[sym] = 8;
		construct(lencode, lens, FIXLCODES);
		for (sym = 0; sym < MAXDCODES; ++sym) lens[sym] = 5;
		construct(distcode, lens, MAXDCODES);
		built = true;
	}
	return codes(s, lencode, distcode);
}

static int dynamicBlock(InflateState& s) {
	static const short order[19] = {
		16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
	int nlen = s.bits(5) + 257;
	int ndist = s.bits(5) + 1;
	int ncode = s.bits(4) + 4;
	if (nlen > MAXLCODES || ndist > MAXDCODES) return -1;
	short lengths[MAXCODES] = { 0 };
	for (int i = 0; i < ncode; ++i) lengths[order[i]] = (short)s.bits(3);
	InflateHuffman lencode;
	if (construct(lencode, lengths, 19) != 0) return -1;
	int idx = 0;
	while (idx < nlen + ndist) {
		int sym = decode(s, lencode);
		if (s.err) return -1;
		if (sym < 16) {
			lengths[idx++] = (short)sym;
		} else if (sym == 16) {
			if (idx == 0) return -1;
			int len = lengths[idx - 1];
			int n = s.bits(2) + 3;
			while (n--) lengths[idx++] = (short)len;
		} else if (sym == 17) {
			int n = s.bits(3) + 3;
			while (n--) lengths[idx++] = 0;
		} else {
			int n = s.bits(7) + 11;
			while (n--) lengths[idx++] = 0;
		}
	}
	if (lengths[256] == 0) return -1;
	InflateHuffman ll;
	if (construct(ll, lengths, nlen) != 0 && (nlen != 1 || lengths[0] != 0)) return -1;
	InflateHuffman dc;
	if (construct(dc, lengths + nlen, ndist) != 0
	    && (ndist != 1 || lengths[nlen] != 0)) return -1;
	return codes(s, ll, dc);
}

bool inflateRaw(const std::uint8_t* in, std::size_t in_len, std::vector<std::uint8_t>& out) {
	InflateState s(in, in_len, out);
	int last;
	do {
		last = s.bits(1);
		int type = s.bits(2);
		if (s.err) return false;
		int rc;
		switch (type) {
		case 0:  rc = stored(s);       break;
		case 1:  rc = fixedBlock(s);   break;
		case 2:  rc = dynamicBlock(s); break;
		default: return false;
		}
		if (rc != 0 || s.err) return false;
	} while (!last);
	return true;
}

}  // namespace wbsh
