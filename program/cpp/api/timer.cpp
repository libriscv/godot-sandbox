#include "timer.hpp"

#include "object.hpp"
#include "syscalls.h"

using TimerEngineCallback = Variant (*)(Variant, Variant);
using TimerEngineNativeCallback = Variant (*)(Object, Variant);
MAKE_SYSCALL(ECALL_TIMER_PERIODIC, void, sys_timer_periodic, Timer::period_t, bool, TimerEngineCallback, void *, Variant *);
MAKE_SYSCALL(ECALL_TIMER_PERIODIC, void, sys_timer_periodic_native, Timer::period_t, bool, TimerEngineNativeCallback, void *, Variant *);
MAKE_SYSCALL(ECALL_TIMER_STOP, void, sys_timer_stop, unsigned);

Variant Timer::create(period_t period, bool oneshot, TimerCallback callback) {
	Variant timer;
	sys_timer_periodic(period, oneshot, [](Variant timer, Variant byte_array) -> Variant {
		auto callback = byte_array.as_byte_array();
		auto *timerfunc = (Timer::TimerCallback *)callback.data();
		return (*timerfunc)(timer);
	}, &callback, &timer);
	return timer;
}

Variant Timer::create_native(period_t period, bool oneshot, TimerNativeCallback callback) {
	Variant timer;
	sys_timer_periodic_native(period, oneshot, [](Object timer, Variant byte_array) -> Variant {
		auto callback = byte_array.as_byte_array();
		auto *timerfunc = (Timer::TimerNativeCallback *)callback.data();
		return (*timerfunc)(timer);
	}, &callback, &timer);
	return timer;
}
