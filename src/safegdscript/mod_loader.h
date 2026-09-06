#pragma once
#if __has_include("safegdscript.h")
#include "safegdscript.h"
#include "scene/main/node.h"
#else
#define SGD_MOD_EXTENSION
#include "script_safegdscript.h"
#include <godot_cpp/classes/node.hpp>
#endif
#include <map>
#include <set>

// One resource and one restricted machine per mod. Trait metadata is read
// before any guest code runs; manifests can only lower the studio's limits.
class SgdModLoader : public Node {
	GDCLASS(SgdModLoader, Node);
	struct Mod {
		Ref<SafeGDScript> script;
		ObjectID node;
		ObjectID api;
		Sandbox *vm = nullptr;
		std::set<std::string> methods;
		bool quarantined = false;
		uint64_t refusals = 0;
	};
	std::map<String, Mod> mods;
	String last_error;
	int error_line = 0, error_column = 0;
	bool busy = false;
	bool suspended = false;
	bool contract_cached = false, contract_allow_concrete = false;
	PackedStringArray contract_paths, cached_sources;
	std::vector<uint64_t> contract_stamps;
	gdscript::ClassSignature cached_obligations, cached_api;
	bool method_allowed(Object *sandbox, Object *object, const String &method, const String &id);
	Node *fail(const String &id, String reason);
	bool contract(gdscript::ClassSignature &obligations, gdscript::ClassSignature &api,
			PackedStringArray &sources);
protected:
	static void _bind_methods();
	void _notification(int what);
public:
	static void register_settings();
	static String runtime_compiler_marker() {
#if defined(SGD_RUNTIME_COMPILER) || defined(SGD_MOD_EXTENSION)
		return "SGD_MOD_RUNTIME_COMPILER_V1";
#else
		return "SGD_MOD_ELF_ONLY_V1";
#endif
	}
	TypedArray<Dictionary> scan(const PackedStringArray &dirs) const;
	Node *load(const Dictionary &manifest, Object *api, Node *parent) noexcept;
	void unload(const String &id) noexcept;
	void poll_faults() noexcept;
	void clear_contract_cache() { contract_cached = false; }
	Node *get_mod(const String &id) const;
	bool is_quarantined(const String &id) const;
	int64_t get_refusals(const String &id) const;
	String get_last_error() const { return last_error; }
	int get_error_line() const { return error_line; }
	int get_error_column() const { return error_column; }
	~SgdModLoader();
};
