#include "api.hpp"

// Same dictionary loops as bench_micro.gd, via the guest C++ API.
// Measures the floor the compiler can reach. Separate ELF to avoid
// baking the whole compiler during binary translation.

PUBLIC Variant bench_dict_ops(Variant iterations) {
	const long n = iterations;
	Dictionary d = Dictionary::Create();
	for (long i = 0; i < n; i++) {
		d.set(i & 63, i);
	}
	long acc = 0;
	for (long j = 0; j < 64; j++) {
		acc += int64_t(d.get(j));
	}
	return acc;
}

PUBLIC Variant bench_dict_get(Variant iterations) {
	const long n = iterations;
	Dictionary d = Dictionary::Create();
	for (long i = 0; i < 64; i++) {
		d.set(i, i);
	}
	long acc = 0;
	for (long j = 0; j < n; j++) {
		acc += int64_t(d.get(j & 63));
	}
	return acc;
}

PUBLIC Variant bench_dict_string_keys(Variant iterations) {
	const long n = iterations;
	Dictionary d = Dictionary::Create();
	const Variant hp(std::string_view("hp"));
	const Variant mp(std::string_view("mp"));
	d.set(hp, 0);
	d.set(mp, 0);
	for (long i = 0; i < n; i++) {
		d.set(hp, int64_t(d.get(hp)) + 1);
		d.set(mp, int64_t(d.get(mp)) - 1);
	}
	return int64_t(d.get(hp)) + int64_t(d.get(mp));
}

PUBLIC Variant bench_dict_get_default(Variant iterations) {
	const long n = iterations;
	Dictionary d = Dictionary::Create();
	for (long i = 0; i < 64; i++) {
		d.set(i, i);
	}
	long acc = 0;
	for (long j = 0; j < n; j++) {
		acc += int64_t(d("get", j & 127, 0));
	}
	return acc;
}
