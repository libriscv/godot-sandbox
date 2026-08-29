#pragma once
#include "ast.h"
#include <vector>

namespace gdscript {

// Copies concrete trait members into every user and expands each user's `uses`
// list transitively. A script chain calls the second overload on each link before
// merging, with the traits declared anywhere in the compilation available for
// name resolution.
void apply_traits(Program& program);
void apply_traits(Program& program, const std::vector<const TraitDecl*>& available);

} // namespace gdscript
