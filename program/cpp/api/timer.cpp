#include "timer.hpp"

#include "object.hpp"
#include "syscalls.h"

using TimerEngineCallback = Variant (*)(Variant, Variant);
using TimerEngineNativeCallback = Variant (*)(Object, Variant);
MAKE_SYSCALL(ECALL_TIMER_PERIODIC, void, sys_timer_periodic, Timer::period_t, bool, TimerEngineCallback, void *, Variant *);
MAKE_SYSCALL(ECALL_TIMER_PERIODIC, void, sys_timer_periodic_native, Timer::period_t, bool, TimerEngineNativeCallback, void *, Variant *);
MAKE_SYSCALL(ECALL_TIMER_STOP, void, sys_timer_stop, unsigned);

// clang-format off
Variant Timer::create(period_t period, bool oneshot, TimerCallback callback) {
	Variant timer;
	sys_timer_periodic(period, oneshot, [](Variant timer, Variant storage) -> Variant {
		std::vector<uint8_t> callback = storage.as_byte_array().fetch();
		Timer::TimerCallback *timerfunc = (Timer::TimerCallback *)callback.data();
		return (*timerfunc)(timer);
	}, &callback, &timer);
	return timer;
}

Variant Timer::create_native(period_t period, bool oneshot, TimerNativeCallback callback) {
	Variant timer;
	sys_timer_periodic_native(period, oneshot, [](Object timer, Variant storage) -> Variant {
		std::vector<uint8_t> callback = storage.as_byte_array().fetch();
		Timer::TimerNativeCallback *timerfunc = (Timer::TimerNativeCallback *)callback.data();
		return (*timerfunc)(timer);
	}, &callback, &timer);
	return timer;
}
// clang-format on
