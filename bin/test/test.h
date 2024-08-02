#include "syscalls.h"
#include "variant.hpp"
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

extern "C" void empty_function(std::span<Variant> args);
