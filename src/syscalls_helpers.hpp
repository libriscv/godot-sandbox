#pragma once
#include "fast_cast.hpp"
#include <libriscv/machine.hpp>

#define APICALL(func) static void func(machine_t &machine [[maybe_unused]])

#ifdef ENABLE_SYSCALL_TRACE
#define SYS_TRACE(name, result, ...) sys_trace(name, result, ##__VA_ARGS__)
#else
#define SYS_TRACE(name, result, ...)
#endif

namespace riscv {

inline Sandbox &emu(machine_t &m) {
	return *m.get_userdata<Sandbox>();
}

// clang-format off
template <typename Result, typename... Args>
static inline void sys_trace(const String &name, Result result, Args &&...args) {
	char buffer[512];
	char *ptr = buffer;
	ptr += snprintf(ptr, sizeof(buffer), "[TRACE] %s (", name.utf8().ptr());
	([&] {
		if constexpr (std::is_same_v<Args, const char *>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%s", args);
		} else if constexpr (std::is_same_v<Args, String>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%s", args.utf8().ptr());
		} else if constexpr (std::is_same_v<Args, StringName>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%s", String(args).utf8().ptr());
		} else if constexpr (std::is_same_v<Args, GuestVariant *>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "Variant(type=%d %s)", args->type, GuestVariant::type_name(args->type));
		} else if constexpr (std::is_pointer_v<Args>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%p", args);
		} else if constexpr (std::is_floating_point_v<Args>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%f", args);
		} else if constexpr (std::is_same_v<Args, gaddr_t>) {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "0x%lX", long(args));
		} else {
			ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%ld", long(args));
		}
	}(), ...);
	ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), ") -> ");
	if constexpr (std::is_pointer_v<Result>) {
		ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%p", result);
	} else if constexpr (std::is_same_v<Result, String>) {
		ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%s", result.utf8().ptr());
	} else if constexpr (std::is_same_v<Result, Variant>) {
		ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "Variant(type=%d %s)", result.get_type(), GuestVariant::type_name(result.get_type()));
	} else if constexpr (std::is_floating_point_v<Result>) {
		ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%f", result);
	} else if constexpr (std::is_same_v<Result, gaddr_t>) {
		ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "0x%lX", long(result));
	} else {
		ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "%ld", long(result));
	}
	ptr += snprintf(ptr, sizeof(buffer) - (ptr - buffer), "\n");
	if (ptr >= buffer + sizeof(buffer)) {
		ptr = buffer + sizeof(buffer) - 1;
	}
	fwrite(buffer, 1, ptr - buffer, stderr);
	fflush(stderr);
}
// clang-format on

	template <typename T>
	using CppVector = riscv::GuestStdVector<RISCV_ARCH, T>;

	using CppString = riscv::GuestStdString<RISCV_ARCH>;

	/// @brief Fetch a scoped Variant by guest-provided index.
	/// @throw std::runtime_error If the index does not refer to a scoped Variant.
	/// @note Prefer this over get_scoped_variant(idx).value(), which throws a mysterious bad_optional_access.
	static inline const Variant &get_scoped_variant_or_throw(const Sandbox &emu, int32_t idx, const char *what) {
		std::optional<const Variant *> opt = emu.get_scoped_variant(idx);
		if (UNLIKELY(!opt.has_value())) {
			ERR_PRINT(String("Invalid scoped Variant index for ") + what + ": " + itos(idx));
			throw std::runtime_error(std::string("Invalid scoped Variant index for ") + what + ": " + std::to_string(idx));
		}
		return *opt.value();
	}

	static inline void throw_on_call_error(const GDExtensionCallError &error,
			std::string_view method, const String &base_name, const Variant **args, int argc) {
		if (LIKELY(error.error == GDEXTENSION_CALL_OK)) {
			return;
		}
		const String name = String::utf8(method.data(), method.size());
		const String base = (error.error == GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL)
				? String("Nil")
				: base_name;
		String message;
		switch (error.error) {
			case GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT: {
					const char *from = (error.argument >= 0 && error.argument < argc)
						? GuestVariant::type_name(args[error.argument]->get_type())
						: "Variant";
				message = "Invalid type in function '" + name + "' in base '" + base +
						"'. Cannot convert argument " + itos(error.argument + 1) + " from " +
						from + " to " + Variant::get_type_name(Variant::Type(error.expected)) + ".";
				break;
			}
			case GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS:
			case GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS:
				message = "Invalid call to function '" + name + "' in base '" + base +
						"'. Expected " + itos(error.expected) + " argument(s).";
				break;
			case GDEXTENSION_CALL_ERROR_INVALID_METHOD:
			case GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL:
			default:
				message = "Invalid call. Nonexistent function '" + name + "' in base '" +
						String(base) + "'.";
				break;
		}
		throw std::runtime_error(std::string("Variant::call(): ") + message.utf8().get_data());
	}

	// No GDExtension const-ness flag for builtin methods; closed set.
	static inline bool is_container_mutator(std::string_view method) {
		static constexpr std::string_view mutators[] = {
			"append", "append_array", "assign", "clear", "erase", "fill", "insert",
			"make_read_only", "pop_at", "pop_back", "pop_front", "push_back",
			"push_front", "remove_at", "resize", "reverse", "set", "shuffle", "sort",
			"sort_custom",
			"get_or_add", "merge",
		};
		for (const std::string_view m : mutators) {
			if (m == method)
				return true;
		}
		return false;
	}

	/// @note Only Array and Dictionary carry the flag. Converting any other type
	/// yields a fresh empty container, which always reports writable.
	static inline void throw_if_read_only(const Variant &var, const char *what) {
		bool read_only;
		switch (var.get_type()) {
			case Variant::ARRAY:
				read_only = var.operator Array().is_read_only();
				break;
			case Variant::DICTIONARY:
				read_only = var.operator Dictionary().is_read_only();
				break;
			default:
				return;
		}
		if (UNLIKELY(read_only)) {
			throw std::runtime_error(std::string(what) + ": the container is read-only");
		}
	}

	/// @brief View a guest string of len bytes, plus the byte that follows it, so that
	/// the caller can check whether the string is already null-terminated.
	/// @note The length is widened to size_t before the increment, as guests pass 32-bit
	/// lengths: len+1 in 32-bit arithmetic wraps to zero for UINT32_MAX, and memview()
	/// returns an empty view with a null data pointer for zero-length views. The returned
	/// view is always at least 1 byte, so back() and operator[](len) are safe.
	static inline std::string_view memview_with_terminator(machine_t &machine, gaddr_t addr, size_t len) {
		return machine.memory.memview(addr, len + 1);
	}

} // namespace riscv
