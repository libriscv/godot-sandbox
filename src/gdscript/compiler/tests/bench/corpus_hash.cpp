#include "../test_corpus.h"
#include "compiler.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Sha256 {
	uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint8_t block[64] = {};
	size_t filled = 0;
	uint64_t length = 0;

	static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void compress() {
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
		uint32_t w[64];
		for (int i = 0; i < 16; i++) {
			w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16)
				| (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
		}
		for (int i = 16; i < 64; i++) {
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
		for (int i = 0; i < 64; i++) {
			const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			const uint32_t ch = (e & f) ^ (~e & g);
			const uint32_t t1 = h + s1 + ch + k[i] + w[i];
			const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = s0 + maj;
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		state[0] += a; state[1] += b; state[2] += c; state[3] += d;
		state[4] += e; state[5] += f; state[6] += g; state[7] += h;
	}

	void update(const uint8_t* data, size_t size) {
		length += size;
		while (size > 0) {
			const size_t take = std::min(size, sizeof(block) - filled);
			std::memcpy(block + filled, data, take);
			filled += take;
			data += take;
			size -= take;
			if (filled == sizeof(block)) {
				compress();
				filled = 0;
			}
		}
	}

	std::string finish() {
		const uint64_t bits = length * 8;
		const uint8_t one = 0x80;
		update(&one, 1);
		const uint8_t zero = 0;
		while (filled != 56) {
			update(&zero, 1);
		}
		uint8_t tail[8];
		for (int i = 0; i < 8; i++) {
			tail[i] = uint8_t(bits >> (56 - i * 8));
		}
		length -= 8;
		update(tail, 8);

		std::string hex;
		for (uint32_t word : state) {
			char buffer[9];
			std::snprintf(buffer, sizeof(buffer), "%08x", word);
			hex += buffer;
		}
		return hex;
	}
};

std::string hash_of(const std::vector<uint8_t>& data) {
	Sha256 sha;
	sha.update(data.data(), data.size());
	return sha.finish();
}

void emit(const std::string& name, const std::string& source) {
	for (const bool optimize : { true, false }) {
		gdscript::CompilerOptions options;
		options.optimize = optimize;
		gdscript::Compiler compiler;
		const auto elf = compiler.compile(source, options);
		const char* suffix = optimize ? "opt" : "noopt";
		if (elf.empty()) {
			std::printf("%s/%s FAILED %s\n", name.c_str(), suffix, compiler.get_error().c_str());
		} else {
			std::printf("%s/%s %zu %s\n", name.c_str(), suffix, elf.size(), hash_of(elf).c_str());
		}
	}
}

} // namespace

int main(int argc, char** argv) {
	for (const auto& program : gdscript_test::corpus()) {
		emit(std::string("corpus:") + program.name, program.source);
	}
	for (int i = 1; i < argc; i++) {
		FILE* file = std::fopen(argv[i], "rb");
		if (!file) {
			std::fprintf(stderr, "Failed to open %s\n", argv[i]);
			return 1;
		}
		std::string source;
		char chunk[65536];
		size_t got;
		while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
			source.append(chunk, got);
		}
		std::fclose(file);
		emit(argv[i], source);
	}
	return 0;
}
