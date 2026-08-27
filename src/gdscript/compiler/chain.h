#pragma once
#include "ast.h"
#include <string>
#include <vector>

namespace gdscript {

struct ChainLink {
	std::string name;
	std::string path; // diagnostics only
	Program program;
};

// Folds an `extends` chain into one Program. Root first, leaf last.
Program merge_chain(std::vector<ChainLink> links);

inline constexpr size_t MAX_CHAIN_DEPTH = 16;

} // namespace gdscript
