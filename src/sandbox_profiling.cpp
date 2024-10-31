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

Array Sandbox::get_hotspots(const String &elf, int total) const {
	if (!m_profiling_data) {
		ERR_PRINT("Profiling is not currently enabled.");
		return Array();
	}
	ProfilingData &profdata = *m_profiling_data;

	// Gather information about the hotspots
	struct Result {
		gaddr_t pc = 0;
		int count = 0;
		int line = 0;
		String function;
		String file;
	};
	std::vector<Result> results;

	for (const auto &entry : profdata.visited) {
		Result res;
		res.pc = entry.first;
		res.count = entry.second;

		// execute riscv64-linux-gnu-addr2line -e <binary> -f -C 0x<address>
		// using popen() and fgets() to read the output
		const std::string binary = elf.utf8().ptr();
		char buffer[4096];
		snprintf(buffer, sizeof(buffer),
			"riscv64-linux-gnu-addr2line -e %s -f -C 0x%lX", binary.c_str(), long(res.pc));

		FILE * f = popen(buffer, "r");
		if (f) {
			String output;
			while (fgets(buffer, sizeof(buffer), f) != nullptr) {
				output += String::utf8(buffer, strlen(buffer));
			}
			pclose(f);

			// Parse the output. addr2line returns two lines:
			// 1. The function name
			// _physics_process
			// 2. The path to the source file and line number
			// /home/gonzo/github/thp/scenes/objects/trees/trees.cpp:29
			PackedStringArray lines = output.split("\n");
			if (lines.size() >= 2) {
				res.function = lines[0];
				const String &line2 = lines[1];

				// Parse the function name and file/line number
				int idx = line2.rfind(":");
				if (idx != -1) {
					res.file = line2.substr(0, idx);
					res.line = line2.substr(idx + 1).to_int();
				}
			} else {
				res.function = output;
			}
			//printf("Hotspot %zu: %.*s\n", results.size(), int(output.utf8().size()), output.utf8().ptr());
		} else {
			ERR_PRINT("Failed to run riscv64-linux-gnu-addr2line.");
		}

		results.push_back(res);
	}

	// Deduplicate the results, now that we have function names
	HashMap<String, unsigned> dedup;
	for (auto &res : results) {
		if (dedup.has(res.function)) {
			const size_t index = dedup[res.function];
			results[index].count += res.count;
			res.count = 0;
			continue;
		} else {
			dedup.insert(res.function, &res - results.data());
		}
	}

	// Sort the results by frequency
	std::sort(results.begin(), results.end(), [](const Result &a, const Result &b) {
		return a.count > b.count;
	});

	// Remove empty entries
	results.erase(std::remove_if(results.begin(), results.end(), [](const Result &res) {
		return res.count == 0;
	}), results.end());

	// Cut off the results to the top N
	if (total > 0 && results.size() > static_cast<size_t>(total)) {
		results.resize(total);
	}

	Array result;
	for (const Result &res : results) {
		Dictionary hotspot;
		hotspot["function"] = res.function;
		hotspot["file"] = res.file;
		hotspot["line"] = res.line;
		hotspot["count"] = res.count;
		result.push_back(hotspot);
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
