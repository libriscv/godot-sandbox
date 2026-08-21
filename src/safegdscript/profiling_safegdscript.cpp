// Feeds Godot's Script Functions profiler from .sgd self-instrumentation.
// Toggled per-script via the Sandbox `profiling` property (recompile, not runtime).
#include "script_language_safegdscript.h"

#include "../gdscript/compiler/profiling_layout.h"
#include "../sandbox.h"
#include "script_instance_safegdscript.h"
#include "script_safegdscript.h"
#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

// Defined in script_instance_safegdscript.cpp, which owns the map.
SafeGDScript *safegdscript_for_sandbox(const Sandbox *p_sandbox);
void safegdscript_for_each_sandbox(const std::function<void(SafeGDScript &, Sandbox &)> &p_callback);

namespace {

uint64_t nanosecond_rdtime(const riscv::Machine<RISCV_ARCH> &) {
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

struct Counters {
	uint64_t call_count = 0;
	uint64_t self_ns = 0;
	uint64_t total_ns = 0;
};

// Last-frame snapshot for delta computation. Host-side only; guest records grow monotonically.
std::unordered_map<const SafeGDScript *, std::vector<Counters>> g_previous;

// Snapshot at profiling start. Excludes global-initializer work from reports.
std::unordered_map<const SafeGDScript *, std::vector<Counters>> g_baseline;

bool g_collecting = false;

template <typename T>
T read_guest(Sandbox &p_sandbox, gaddr_t p_address) {
	T value {};
	p_sandbox.machine().copy_from_guest(&value, p_address, sizeof(T));
	return value;
}

std::vector<Counters> read_records(SafeGDScript &p_script, Sandbox &p_sandbox) {
	std::vector<Counters> records;
	if (!p_script.is_profiled_build() || !p_sandbox.has_program_loaded()) {
		return records;
	}
	const gaddr_t base = p_sandbox.address_of(gdscript::PROFILING_SYMBOL);
	if (base == 0) {
		return records;
	}
	if (read_guest<uint32_t>(p_sandbox, base + gdscript::ProfilingLayout::MAGIC_OFF) != gdscript::ProfilingLayout::MAGIC ||
			read_guest<uint32_t>(p_sandbox, base + gdscript::ProfilingLayout::VERSION_OFF) != gdscript::ProfilingLayout::LAYOUT_VERSION) {
		return records;
	}
	const uint32_t count = read_guest<uint32_t>(p_sandbox, base + gdscript::ProfilingLayout::FUNCTION_COUNT_OFF);
	// Record count must match signature table; a mismatch means stale data.
	if (count != p_script.get_signatures().size()) {
		return records;
	}
	records.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const gaddr_t record = base + gdscript::ProfilingLayout::record_offset(i);
		records[i].call_count = read_guest<uint64_t>(p_sandbox, record + gdscript::ProfilingLayout::CALL_COUNT_OFF);
		records[i].self_ns = read_guest<uint64_t>(p_sandbox, record + gdscript::ProfilingLayout::SELF_OFF);
		records[i].total_ns = read_guest<uint64_t>(p_sandbox, record + gdscript::ProfilingLayout::TOTAL_OFF);
	}
	return records;
}

// "res://path.sgd::LINE::name" — editor splits this to jump to the function.
StringName profiler_signature(const SafeGDScript &p_script, const gdscript::FunctionSignature &p_signature) {
	const String name = String::utf8(p_signature.name.c_str(), p_signature.name.size());
	return StringName(p_script.get_path() + String("::") + itos(p_signature.line) + String("::") + name);
}

// godot-cpp's generated ProfilingInfo is missing internal_time (32 vs 40 bytes),
// so indexing the engine's array with it misaligns every entry past the first.
struct EngineProfilingInfo {
	StringName signature;
	uint64_t call_count;
	uint64_t total_time;
	uint64_t self_time;
	uint64_t internal_time;
};
static_assert(sizeof(EngineProfilingInfo) >= sizeof(ScriptLanguageExtensionProfilingInfo),
		"ScriptLanguageExtensionProfilingInfo grew past the engine's ProfilingInfo");

uint64_t to_usec(uint64_t p_nanoseconds) {
	return p_nanoseconds / 1000;
}

int32_t collect(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max, bool p_delta) {
	if (p_info_array == nullptr || p_info_max <= 0) {
		return 0;
	}
	EngineProfilingInfo *const engine_array = reinterpret_cast<EngineProfilingInfo *>(p_info_array);
	int32_t written = 0;
	safegdscript_for_each_sandbox([&](SafeGDScript &script, Sandbox &sandbox) {
		if (written >= p_info_max) {
			return;
		}
		const std::vector<Counters> records = read_records(script, sandbox);
		if (records.empty()) {
			return;
		}
		std::vector<Counters> &previous = g_previous[&script];
		std::vector<Counters> &baseline = g_baseline[&script];
		previous.resize(records.size());
		baseline.resize(records.size());

		for (size_t i = 0; i < records.size() && written < p_info_max; i++) {
			const Counters &now = records[i];
			const Counters &before = p_delta ? previous[i] : baseline[i];
			const Counters shown {
				now.call_count - before.call_count,
				now.self_ns - before.self_ns,
				now.total_ns - before.total_ns,
			};
			if (shown.call_count == 0) {
				continue;
			}
			EngineProfilingInfo &info = engine_array[written++];
			info.signature = profiler_signature(script, script.get_signatures()[i]);
			info.call_count = shown.call_count;
			info.self_time = to_usec(shown.self_ns);
			info.total_time = to_usec(shown.total_ns);
			info.internal_time = 0;
		}
		if (p_delta) {
			previous = records;
		}
	});
	return written;
}

} // namespace

void safegdscript_sandbox_profiling_toggled(Sandbox &p_sandbox, bool p_enabled) {
	SafeGDScript *script = safegdscript_for_sandbox(&p_sandbox);
	if (script == nullptr || script->is_profiled_build() == p_enabled) {
		return;
	}
	// Clock installed after rebuild; baseline below excludes init work.
	script->compile_source_to_elf(p_enabled);
	g_previous.erase(script);
	g_baseline.erase(script);
	if (!p_enabled) {
		return;
	}
	p_sandbox.machine().set_rdtime(nanosecond_rdtime);
	const std::vector<Counters> started_at = read_records(*script, p_sandbox);
	g_previous[script] = started_at;
	g_baseline[script] = started_at;
}

void SafeGDScriptLanguage::_profiling_start() {
	g_collecting = true;
	safegdscript_for_each_sandbox([](SafeGDScript &script, Sandbox &sandbox) {
		const std::vector<Counters> started_at = read_records(script, sandbox);
		g_previous[&script] = started_at;
		g_baseline[&script] = started_at;
	});
}

void SafeGDScriptLanguage::_profiling_stop() {
	g_collecting = false;
	g_previous.clear();
	g_baseline.clear();
}

// Syscall time is part of the caller's self time; nothing to toggle.
void SafeGDScriptLanguage::_profiling_set_save_native_calls(bool p_enable) {
}

int32_t SafeGDScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	if (!g_collecting) {
		return 0;
	}
	return collect(p_info_array, p_info_max, false);
}

int32_t SafeGDScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	if (!g_collecting) {
		return 0;
	}
	return collect(p_info_array, p_info_max, true);
}
