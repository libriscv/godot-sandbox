#include <api.hpp>
#include <string>
#include <vector>

// Include the GDScript compiler
#include "../../../../src/gdscript/compiler/compiler.h"

static Variant compile_gdscript(String source_code) {
	try {
		gdscript::Compiler compiler;
		gdscript::CompilerOptions options;
		options.optimize = true;

		std::string source = source_code.utf8().c_str();
		std::vector<uint8_t> elf_data = compiler.compile(source, options);

		// Return the compiled ELF data as a PackedByteArray
		PackedByteArray result;
		result.resize(elf_data.size());
		memcpy(result.ptrw(), elf_data.data(), elf_data.size());
		return result;
	} catch (const std::exception& e) {
		print("GDScript compilation error: ", e.what());
		return Variant(); // Return null on error
	}
}

int main() {
	print("GDScript Compiler ELF loaded");

	// Register the compile function that SafeGDScript will call
	ADD_API_FUNCTION(compile_gdscript, "PackedByteArray", "String source_code", "Compiles GDScript source code to ELF binary");

	halt();
}
