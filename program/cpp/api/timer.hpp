#pragma once

#include "function.hpp"
#include "variant.hpp"

struct Timer {
	using period_t = double;
	using TimerCallback = Function<Variant(Variant)>;

	static Variant oneshot(period_t secs, TimerCallback callback);

	static Variant periodic(period_t period, TimerCallback callback);

private:
	static Variant create(period_t p, bool oneshot, TimerCallback callback);
};

inline Variant Timer::oneshot(period_t secs, TimerCallback callback) {
	return create(secs, true, callback);
}

inline Variant Timer::periodic(period_t period, TimerCallback callback) {
	return create(period, false, callback);
}
