#include "api.hpp"

// Must stay separate: all-numeric defaults so no side-effect keeps the
// properties array alive. A String/Object default masks GH #272.
static float speed = 400.0f;
static int lives = 3;

// clang-format off
SANDBOXED_PROPERTIES(2, {
	.name = "speed",
	.type = Variant::FLOAT,
	.getter = []() -> Variant { return speed; },
	.setter = [](Variant value) -> Variant { return speed = value; },
	.default_value = Variant{400.0},
}, {
	.name = "lives",
	.type = Variant::INT,
	.getter = []() -> Variant { return lives; },
	.setter = [](Variant value) -> Variant { return lives = value; },
	.default_value = Variant{3},
});
// clang-format on
