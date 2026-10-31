// A small self-contained PNG writer (8-bit RGB or RGBA): the "Up" row filter, then deflate with fixed Huffman codes and greedy LZ77
// matches (hash chains, 32 KB window). Enough for flat LCD artwork, and needs no zlib on the device (vst/skin_recipe.h).
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace md::png
{
	namespace detail
	{
		inline uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0)
		{
			static const auto table = []
			{
				std::array<uint32_t, 256> t{};
				for(uint32_t i = 0; i < 256; ++i)
				{
					uint32_t v = i;
					for(int k = 0; k < 8; ++k) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
					t[i] = v;
				}
				return t;
			}();
			c = ~c;
			for(size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
			return ~c;
		}

		struct Bits
		{
			std::vector<uint8_t> out;
			uint32_t acc = 0;
			int n = 0;
			void put(uint32_t v, int bits) { acc |= v << n; n += bits; while(n >= 8) { out.push_back(uint8_t(acc)); acc >>= 8; n -= 8; } }
			void putRev(uint32_t code, int bits) { uint32_t r = 0; for(int i = 0; i < bits; ++i) r |= ((code >> i) & 1) << (bits - 1 - i); put(r, bits); }	// Huffman codes MSB first
			void flush() { if(n) out.push_back(uint8_t(acc)); acc = 0; n = 0; }
		};

		inline void literal(Bits& b, int v)	// fixed Huffman literal/length codes
		{
			if(v < 144) b.putRev(0x30 + v, 8);
			else if(v < 256) b.putRev(0x190 + v - 144, 9);
			else if(v < 280) b.putRev(v - 256, 7);
			else b.putRev(0xc0 + v - 280, 8);
		}

		inline void match(Bits& b, int len, int dist)
		{
			static const int lb[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
			static const int le[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
			static const int db[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
			static const int de[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
			int i = 28;
			while(lb[i] > len) --i;
			literal(b, 257 + i);
			if(le[i]) b.put(uint32_t(len - lb[i]), le[i]);
			int j = 29;
			while(db[j] > dist) --j;
			b.putRev(uint32_t(j), 5);
			if(de[j]) b.put(uint32_t(dist - db[j]), de[j]);
		}

		inline std::vector<uint8_t> zlib(const std::vector<uint8_t>& in)
		{
			Bits b;
			b.out = {0x78, 0x01};
			b.put(1, 1);	// last block
			b.put(1, 2);	// fixed Huffman
			constexpr int kHash = 1 << 15, kWindow = 32768, kChain = 32;
			std::vector<int> head(kHash, -1), prev(in.size(), -1);
			auto hash = [&](size_t i) { return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & (kHash - 1); };
			size_t i = 0;
			while(i < in.size())
			{
				int best = 0, dist = 0;
				if(i + 2 < in.size())
				{
					const int h = hash(i);
					int c = head[size_t(h)];
					for(int k = 0; k < kChain && c >= 0 && int(i) - c <= kWindow; ++k, c = prev[size_t(c)])
					{
						int l = 0;
						while(l < 258 && i + size_t(l) < in.size() && in[size_t(c) + size_t(l)] == in[i + size_t(l)]) ++l;
						if(l > best) { best = l; dist = int(i) - c; if(l == 258) break; }
					}
				}
				const size_t step = best >= 3 ? size_t(best) : 1;
				if(best >= 3) match(b, best, dist); else literal(b, in[i]);
				for(size_t k = 0; k < step; ++k, ++i)
					if(i + 2 < in.size()) { const int h = hash(i); prev[i] = head[size_t(h)]; head[size_t(h)] = int(i); }
			}
			literal(b, 256);
			b.flush();
			uint32_t s1 = 1, s2 = 0;
			for(uint8_t v : in) { s1 = (s1 + v) % 65521; s2 = (s2 + s1) % 65521; }
			const uint32_t ad = (s2 << 16) | s1;
			for(int k = 3; k >= 0; --k) b.out.push_back(uint8_t(ad >> (8 * k)));
			return b.out;
		}

		inline void chunk(std::vector<uint8_t>& f, const char* type, const std::vector<uint8_t>& data)
		{
			const uint32_t n = uint32_t(data.size());
			for(int k = 3; k >= 0; --k) f.push_back(uint8_t(n >> (8 * k)));
			const size_t start = f.size();
			f.insert(f.end(), type, type + 4);
			f.insert(f.end(), data.begin(), data.end());
			const uint32_t c = crc(f.data() + start, f.size() - start);
			for(int k = 3; k >= 0; --k) f.push_back(uint8_t(c >> (8 * k)));
		}
	}

	// px: w*h pixels of 4 bytes RGBA; written as RGB when alpha is false (the alpha bytes are dropped).
	inline bool write(const std::string& path, const uint8_t* px, uint32_t w, uint32_t h, bool alpha)
	{
		const int bpp = alpha ? 4 : 3;
		std::vector<uint8_t> raw;
		raw.reserve(size_t(h) * (1 + size_t(w) * size_t(bpp)));
		std::vector<uint8_t> line(size_t(w) * size_t(bpp)), up(line.size(), 0);
		for(uint32_t y = 0; y < h; ++y)
		{
			for(uint32_t x = 0; x < w; ++x)
				for(int c = 0; c < bpp; ++c) line[size_t(x) * size_t(bpp) + size_t(c)] = px[(size_t(y) * w + x) * 4 + size_t(c)];
			raw.push_back(2);	// Up
			for(size_t i = 0; i < line.size(); ++i) raw.push_back(uint8_t(line[i] - up[i]));
			up.swap(line);
		}
		std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		std::vector<uint8_t> ihdr(13);
		for(int k = 0; k < 4; ++k) { ihdr[size_t(k)] = uint8_t(w >> (24 - 8 * k)); ihdr[size_t(4 + k)] = uint8_t(h >> (24 - 8 * k)); }
		ihdr[8] = 8;
		ihdr[9] = alpha ? 6 : 2;
		detail::chunk(f, "IHDR", ihdr);
		detail::chunk(f, "IDAT", detail::zlib(raw));
		detail::chunk(f, "IEND", {});
		std::FILE* fp = std::fopen(path.c_str(), "wb");
		if(!fp) return false;
		const bool ok = std::fwrite(f.data(), 1, f.size(), fp) == f.size();
		return std::fclose(fp) == 0 && ok;
	}
}
