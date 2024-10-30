#include "sandbox.h"

#include <algorithm>
#include <godot_cpp/variant/utility_functions.hpp>

void Sandbox::set_profiling(bool enable) {
	enable_profiling(enable);
}

void Sandbox::enable_profiling(bool enable, uint32_t interval) {
	if (enable) {
		if (!m_profiling_data) {
			m_profiling_data = std::make_unique<ProfilingData>();
		}
		m_profiling_data->profiling_interval = interval;
	} else {
		if (this->is_in_vmcall()) {
			ERR_PRINT("Cannot disable profiling while a VM call is in progress.");
			return;
		}
		m_profiling_data.reset();
	}
}

String Sandbox::get_hotspots(const String &elf, int total) const {
	if (!m_profiling_data) {
		ERR_PRINT("Profiling is not currently enabled.");
		return "";
	}
	ProfilingData &profdata = *m_profiling_data;

	// Print the visited addresses sorted by frequency
	std::vector<std::pair<gaddr_t, int>> sorted;
	sorted.reserve(profdata.visited.size());
	for (const auto &[pc, count] : profdata.visited) {
		sorted.push_back({ pc, count });
	}
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
		return a.second > b.second;
	});

	String result;

	for (size_t i = 0; i < std::min(sorted.size(), size_t(total)); i++) {
		const auto &[pc, count] = sorted[i];
		char buffer[512];
		const int len =
			snprintf(buffer, sizeof(buffer), "[0x%08lX] Count: %d \t", long(pc), count);
		result += String::utf8(buffer, len);
		// execute riscv64-linux-gnu-addr2line -e <binary> -f -C -i 0x<address>
		// using popen() and fgets() to read the output
		const std::string binary = elf.utf8().ptr();
		snprintf(buffer, sizeof(buffer),
			"riscv64-linux-gnu-addr2line -e %s -f -C -i 0x%lX", binary.c_str(), long(pc));

		FILE * f = popen(buffer, "r");
		if (f) {
			while (fgets(buffer, sizeof(buffer), f) != nullptr) {
				result += String::utf8(buffer, strlen(buffer));
			}
			pclose(f);
		}
	}

	return result;
}

void Sandbox::clear_hotspots() const {
	if (!m_profiling_data) {
		ERR_PRINT("Profiling is not currently enabled.");
		return;
	}
	m_profiling_data->visited.clear();
}
