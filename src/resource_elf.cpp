#include "resource_elf.h"

#include <godot_cpp/classes/file_access.hpp>

PackedByteArray ELFResource::get_content() {
	String p_path = get_file();
	return FileAccess::get_file_as_bytes(p_path);
}
