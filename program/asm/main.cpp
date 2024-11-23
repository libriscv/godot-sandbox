#include <api.hpp>
#include <string>
using AsmCallback = Variant(*)();
extern AsmCallback assemble_to(const std::string& input);

extern "C"
Variant assemble(String input) {
	AsmCallback callback = assemble_to(input.utf8());
	if (!callback) {
		fprintf(stderr, "Failed to assemble\n");
		return Nil;
	}
	return Callable::Create<Variant()>(callback);
}
