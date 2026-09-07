#include "mod_loader.h"
#ifdef SGD_MOD_EXTENSION
#include "../sandbox.h"
#include "../gdscript/compiler/trait_conformance.h"
#include "compiler_backend.h"
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#else
#include "../core/sandbox.h"
#include "../contract/trait_conformance.h"
#include "core/config/project_settings.h"
#include "scene/2d/node_2d.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#ifdef SGD_RUNTIME_COMPILER
#include "../editor/compiler_backend.h"
#endif
#endif
#include <algorithm>

namespace {
Variant setting(const char *name, const Variant &fallback) {
	auto *s = ProjectSettings::get_singleton();
	return s && s->has_setting(name) ? s->get_setting(name) : fallback;
}
String entry_form(const String &entry) {
	String form = setting("sandbox/mods/entry_form", "auto");
	if (form != "auto") return form;
	if (entry.get_extension() == "elf") return "elf";
#if defined(SGD_RUNTIME_COMPILER) || defined(SGD_MOD_EXTENSION)
	return "source";
#else
	return "elf";
#endif
}
const char *limit_names[] = { "execution_timeout", "memory_max", "allocations_max", "references_max", "coroutines_max" };
const int64_t defaults[] = { 1, 32, 8000, 100, 32 };
String text(const std::string &s) { return String::utf8(s.c_str()); }
bool native_subtype(const std::string &actual, const std::string &required) {
	return ClassDB::class_exists(text(actual)) && ClassDB::class_exists(text(required)) &&
		ClassDB::is_parent_class(text(actual), text(required));
}
gdscript::DeclaredType reflected_type(const Dictionary &info) {
	gdscript::DeclaredType result;
	const int type = int64_t(info["type"]);
	if (type == Variant::NIL && (int64_t(info.get("usage", 0)) & PROPERTY_USAGE_NIL_IS_VARIANT)) {
		result.name = "Variant";
		return result;
	}
	result.mask = uint64_t(1) << type;
	String name = type == Variant::NIL ? String("void") : Variant::get_type_name(Variant::Type(type));
	const String class_name = info.get("class_name", "");
	if (type == Variant::OBJECT && !class_name.is_empty()) name = class_name;
	const String hint = info.get("hint_string", "");
	if (!hint.is_empty() && (type == Variant::ARRAY || type == Variant::DICTIONARY)) {
		name += "[" + hint.replace(";", ", ") + "]";
	}
	result.name = name.utf8().get_data();
	return result;
}
String identifier(const String &name) {
	std::string result;
	for (int i = 0; i < name.length(); ++i) {
		const char32_t c = name[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		result += ok ? char(c) : '_';
	}
	if (result.empty() || (result[0] >= '0' && result[0] <= '9')) result.insert(result.begin(), '_');
	return text(result);
}
bool read_bytes(const String &path, PackedByteArray &bytes, uint64_t maximum) {
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null() || file->get_length() == 0 || file->get_length() > maximum) return false;
	bytes = file->get_buffer(file->get_length());
	return !bytes.is_empty();
}
void callbacks(Node *node, const Ref<SafeGDScript> &script, bool enable) {
	auto declares = [&](const char *name) {
		for (const auto &s : script->get_metadata().functions) if (s.name == name && !s.is_static && s.parameters.size() == 1) return enable;
		return false;
	};
	node->set_process(declares("_process"));
	node->set_physics_process(declares("_physics_process"));
	node->set_process_input(declares("_input"));
	node->set_process_shortcut_input(declares("_shortcut_input"));
	node->set_process_unhandled_input(declares("_unhandled_input"));
	node->set_process_unhandled_key_input(declares("_unhandled_key_input"));
}
}
void SgdModLoader::register_settings() {
	auto *s = ProjectSettings::get_singleton();
	auto def = [&](const String &key, const Variant &value) {
		if (!s->has_setting(key)) s->set_setting(key, value);
		s->set_initial_value(key, value);
	};
	PackedStringArray dirs; dirs.push_back("res://mods"); dirs.push_back("user://mods");
	def("sandbox/mods/directories", dirs);
	def("sandbox/mods/obligations_trait", "");
	def("sandbox/mods/api_trait", "");
	def("sandbox/mods/entry_form", "auto");
#ifndef SGD_MOD_EXTENSION
	// Exporter settings only exist in the module, which owns that exporter.
	def("sandbox/mods/runtime_compiler_template", "");
	def("sandbox/mods/restricted", "mods/**");
#endif
	def("sandbox/mods/node_type", "Node");
	for (size_t i = 0; i < 5; ++i) def(String("sandbox/mods/limits/") + limit_names[i], defaults[i]);
}
void SgdModLoader::_bind_methods() {
	ClassDB::bind_static_method("SgdModLoader", D_METHOD("runtime_compiler_marker"), &SgdModLoader::runtime_compiler_marker);
	ClassDB::bind_method(D_METHOD("scan", "dirs"), &SgdModLoader::scan, DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("load", "mod", "api", "parent"), static_cast<Node *(SgdModLoader::*)(const Dictionary &, Object *, Node *)>(&SgdModLoader::load));
	ClassDB::bind_method(D_METHOD("unload", "id"), static_cast<void (SgdModLoader::*)(const String &)>(&SgdModLoader::unload));
	ClassDB::bind_method(D_METHOD("poll_faults"), static_cast<void (SgdModLoader::*)()>(&SgdModLoader::poll_faults));
	ClassDB::bind_method(D_METHOD("clear_contract_cache"), &SgdModLoader::clear_contract_cache);
	ClassDB::bind_method(D_METHOD("get_mod", "id"), &SgdModLoader::get_mod);
	ClassDB::bind_method(D_METHOD("is_quarantined", "id"), &SgdModLoader::is_quarantined);
	ClassDB::bind_method(D_METHOD("get_refusals", "id"), &SgdModLoader::get_refusals);
	ClassDB::bind_method(D_METHOD("get_last_error"), &SgdModLoader::get_last_error);
	ClassDB::bind_method(D_METHOD("get_error_line"), &SgdModLoader::get_error_line);
	ClassDB::bind_method(D_METHOD("get_error_column"), &SgdModLoader::get_error_column);
	ClassDB::bind_method(D_METHOD("_method_allowed", "sandbox", "object", "method", "id"), &SgdModLoader::method_allowed);
	ADD_SIGNAL(MethodInfo("mod_loaded", PropertyInfo(Variant::STRING, "id")));
	ADD_SIGNAL(MethodInfo("mod_failed", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("mod_faulted", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "reason"), PropertyInfo(Variant::STRING, "location")));
	ADD_SIGNAL(MethodInfo("mod_refused", PropertyInfo(Variant::STRING, "id"), PropertyInfo(Variant::STRING, "method")));
}
TypedArray<Dictionary> SgdModLoader::scan(const PackedStringArray &dirs) const {
	PackedStringArray roots = dirs.is_empty() ? PackedStringArray(setting("sandbox/mods/directories", PackedStringArray())) : dirs;
	std::vector<Dictionary> found;
	for (const String &root : roots) {
		Ref<DirAccess> dir = DirAccess::open(root);
		if (dir.is_null()) continue;
		dir->list_dir_begin();
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (!dir->current_is_dir() || name.begins_with(".")) continue;
			Ref<ConfigFile> cfg; cfg.instantiate();
			String path = root.path_join(name);
			if (cfg->load(path.path_join("mod.cfg")) != OK) continue;
			Dictionary mod;
			mod["dir"] = path;
			for (const char *key : {"id", "name", "version", "entry", "order"}) {
				Variant fallback = String();
				if (String(key) == "id") fallback = identifier(name);
				if (String(key) == "name") fallback = name;
				if (String(key) == "version") fallback = "0";
				if (String(key) == "order") fallback = 0;
				mod[key] = cfg->get_value("mod", key, fallback);
			}
			Dictionary limits;
			for (const char *key : limit_names) if (cfg->has_section_key("limits", key)) limits[key] = cfg->get_value("limits", key);
			mod["limits"] = limits; found.push_back(mod);
		}
		dir->list_dir_end();
	}
	std::sort(found.begin(), found.end(), [](const Dictionary &a, const Dictionary &b) {
		const int64_t x = a["order"], y = b["order"];
		return x == y ? String(a["id"]) < String(b["id"]) : x < y;
	});
	TypedArray<Dictionary> result;
	for (const auto &mod : found) result.push_back(mod);
	return result;
}
bool SgdModLoader::contract(gdscript::ClassSignature &obligations, gdscript::ClassSignature &api, PackedStringArray &sources) {
	PackedStringArray paths;
	std::vector<uint64_t> stamps;
	for (const char *key : {"sandbox/mods/obligations_trait", "sandbox/mods/api_trait"}) {
		String path = setting(key, "");
#if !defined(SGD_RUNTIME_COMPILER) && !defined(SGD_MOD_EXTENSION)
		if (!path.is_empty() && path.get_extension() != "elf") path += ".elf";
#endif
		paths.push_back(path);
		stamps.push_back(FileAccess::get_modified_time(path));
	}
	if (contract_cached && paths == contract_paths && stamps == contract_stamps) {
		obligations = cached_obligations; api = cached_api; sources = cached_sources;
		return true;
	}
	contract_cached = false;
	std::vector<std::pair<String, PackedByteArray>> files;
	for (const char *key : {"sandbox/mods/obligations_trait", "sandbox/mods/api_trait"}) {
		String path = setting(key, "");
#if !defined(SGD_RUNTIME_COMPILER) && !defined(SGD_MOD_EXTENSION)
		if (!path.is_empty() && path.get_extension() != "elf") path += ".elf";
#endif
		PackedByteArray bytes;
		if (path.is_empty() || !read_bytes(path, bytes, 256 * 1024)) { last_error = String("Cannot read ") + key + ": " + path; return false; }
		files.push_back({path, bytes});
#if defined(SGD_RUNTIME_COMPILER) || defined(SGD_MOD_EXTENSION)
		if (path.get_extension() != "elf") {
			String source = String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size());
			String name;
			bool is_trait = false;
			SafeGDScript::scan_class_header(source, &name, nullptr, &is_trait);
			if (name.is_empty() || !is_trait) { last_error = "SDK file must declare trait_name: " + path; return false; }
			sources.push_back("trait:" + name); sources.push_back(path); sources.push_back(source);
		}
#endif
	}
	for (size_t i = 0; i < files.size(); ++i) {
		auto bytes = files[i].second;
#if defined(SGD_RUNTIME_COMPILER) || defined(SGD_MOD_EXTENSION)
		if (files[i].first.get_extension() != "elf") {
			auto &compiler = gdscript_compiler::backend_for(false);
			PackedStringArray dependencies;
			for (int j = 0; j + 2 < sources.size(); j += 3) if (sources[j + 1] != files[i].first) {
				dependencies.push_back(sources[j]); dependencies.push_back(sources[j + 1]); dependencies.push_back(sources[j + 2]);
			}
			gdscript_compiler::prepare(compiler, false, dependencies, files[i].first);
			GDScriptCompilerBackend::BuildOptions options; options.debug = true;
			bytes = compiler.compile(String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()), options);
			if (bytes.is_empty()) { last_error = compiler.error_message(); return false; }
		}
#endif
		const auto info = Sandbox::get_program_info_from_binary(bytes);
		if (!info.has_script_metadata) { last_error = "SDK requires trait ELF metadata (source requires the runtime compiler): " + files[i].first; return false; }
		// A trait-only file publishes its own declaration last, after dependencies.
		const gdscript::ClassSignature *selected = nullptr;
		for (const auto &c : info.script_metadata.classes) if (c.is_trait && c.name == info.script_metadata.class_name) selected = &c;
		if (!selected) { last_error = "SDK file has no trait declaration: " + files[i].first; return false; }
		(i == 0 ? obligations : api) = *selected;
	}
	contract_paths = paths; contract_stamps = stamps;
	cached_obligations = obligations; cached_api = api; cached_sources = sources;
	contract_cached = true;
	return true;
}
Node *SgdModLoader::fail(const String &id, String reason) {
	last_error = reason;
	emit_signal("mod_failed", id, reason);
	return nullptr;
}
bool SgdModLoader::method_allowed(Object *, Object *object, const String &method, const String &id) {
	auto it = mods.find(id);
	if (it == mods.end() || it->second.quarantined || suspended) return false;
	Mod &mod = it->second;
	// The owner is a granted capability too; API calls retain the trait allow-list.
	if (object && uint64_t(object->get_instance_id()) == uint64_t(mod.node)
			&& method != "free" && method != "set_script"
			&& !method.begins_with("set_process") && !method.begins_with("set_physics_process")) return true;
	if (object && uint64_t(object->get_instance_id()) == uint64_t(mod.api) && mod.methods.count(method.utf8().get_data())) return true;
	++mod.refusals;
	// Defer host signals: listeners must not mutate the loader during a guest call.
	call_deferred("emit_signal", "mod_refused", id, method);
	return false;
}
Node *SgdModLoader::load(const Dictionary &manifest, Object *api, Node *parent) noexcept try {
	const String id = manifest.get("id", "");
	if (busy) return fail(id, "The mod loader is already loading or unloading a mod");
	struct Guard { bool &flag; bool held = true; Guard(bool &f):flag(f) { flag=true; } void release() { if (held) { flag=false; held=false; } } ~Guard(){release();} } guard(busy);
	last_error = String(); error_line = error_column = 0;
	if (!id.is_valid_ascii_identifier() || mods.count(id)) return fail(id, "Invalid or duplicate mod id: " + id);
	if (!api || !parent) return fail(id, "An API object and parent node are required");
	const String entry = manifest.get("entry", "");
	const String form = entry_form(entry);
	if (form != "source" && form != "elf") return fail(id, "sandbox/mods/entry_form must be auto, source or elf");
	if (entry.is_empty() || entry.get_file() != entry || entry.contains(":") || entry.contains("\\") || (form == "source" ? entry.get_extension() != "sgd" : entry.get_extension() != "elf")) return fail(id, "Entry must name a " + form + " file directly in the mod folder");
	gdscript::ClassSignature obligations, offered;
	PackedStringArray sources;
	if (!contract(obligations, offered, sources)) return fail(id, last_error);
	// Check the host object before granting it. Native MethodInfo and Script
	// signatures share Variant IDs; SafeGDScript retains nominal declarations.
	Ref<SafeGDScript> api_script = api->get_script();
	if (api_script.is_valid()) {
		const auto errors = gdscript::trait_conformance(api_script->get_metadata().functions, offered, native_subtype);
		if (!errors.empty()) return fail(id, "API object: " + text(errors.front()));
	} else {
		std::vector<gdscript::FunctionSignature> functions;
		Object *api_script_object = api->get_script();
		const Array descriptions = api_script_object ? api_script_object->call("get_script_method_list") : api->call("get_method_list");
		for (int i = 0; i < descriptions.size(); ++i) {
			Dictionary method = descriptions[i];
			gdscript::FunctionSignature sig;
			sig.has_declaration = true;
			sig.name = String(method["name"]).utf8().get_data();
			sig.is_static = (int64_t(method.get("flags", 0)) & METHOD_FLAG_STATIC) != 0;
			Dictionary ret = method["return"];
			sig.declared_return = reflected_type(ret);
			sig.return_type = int64_t(ret["type"]);
			if (sig.return_type == 0 && (int64_t(ret.get("usage", 0)) & PROPERTY_USAGE_NIL_IS_VARIANT)) sig.return_type = -1;
			Array arguments = method["args"];
			for (int j = 0; j < arguments.size(); ++j) {
				Dictionary arg = arguments[j];
				gdscript::FunctionParameter parameter;
				parameter.declared_type = reflected_type(arg);
				parameter.name = String(arg["name"]).utf8().get_data();
				parameter.type = int64_t(arg["type"]);
				if (parameter.type == 0 && (int64_t(arg.get("usage", 0)) & PROPERTY_USAGE_NIL_IS_VARIANT)) parameter.type = -1;
				sig.parameters.push_back(parameter);
			}
			functions.push_back(sig);
		}
		const auto errors = gdscript::trait_conformance(functions, offered, native_subtype);
		if (!errors.empty()) return fail(id, "API object: " + text(errors.front()));
	}
	PackedByteArray bytes;
	const String path = String(manifest.get("dir", "res://mods/" + id)).path_join(entry);
	if (form == "source" && manifest.has("source")) bytes = String(manifest["source"]).to_utf8_buffer();
	else if (!read_bytes(path, bytes, form == "source" ? 256 * 1024 : 16 * 1024 * 1024)) return fail(id, "Missing, empty or oversized mod entry: " + path);
	if (form == "source") {
#if defined(SGD_RUNTIME_COMPILER) || defined(SGD_MOD_EXTENSION)
		if (bytes.size() > 256 * 1024) return fail(id, "Mod source exceeds 256 KiB");
		auto &compiler = gdscript_compiler::backend_for(true);
		gdscript_compiler::prepare(compiler, true, sources, path);
		GDScriptCompilerBackend::BuildOptions options; options.debug = true;
		bytes = compiler.compile(String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()), options);
		if (bytes.is_empty()) {
#ifndef SGD_MOD_EXTENSION
			error_line = compiler.error_line(); error_column = compiler.error_column();
#endif
			return fail(id, compiler.error_message());
		}
#else
		return fail(id, "Source mods require a template built with sgd_runtime_compiler=yes");
#endif
	}
	Ref<SafeGDScript> script; script.instantiate();
	if (!script->load_binary(bytes)) return fail(id, "Invalid mod ELF metadata");
	script->set_mod_source_path(path);
	// Validate what this ELF was compiled to call, not just the host's current
	// API object. Additions and signature-compatible changes need no rebuild.
	// Independently compiled, untyped mods may have no recorded API trait.
	for (const auto &expected : script->get_metadata().classes) {
		if (!expected.is_trait || expected.name != offered.name) continue;
		const auto errors = gdscript::trait_conformance(offered.trait_methods, expected, native_subtype);
		if (!errors.empty()) return fail(id, "Mod '" + id + "' requires an incompatible game API: " + text(errors.front()));
	}
	// Concrete callbacks are opt-in for already compiled mods
	const auto &functions = script->get_metadata().functions;
	const auto hook_errors = gdscript::required_host_hooks(functions, obligations, native_subtype);
	if (!hook_errors.empty()) return fail(id, "Mod '" + id + "' is incompatible: " + text(hook_errors.front()));
	obligations.trait_methods.erase(std::remove_if(obligations.trait_methods.begin(), obligations.trait_methods.end(), [&](const auto &method) {
		return !method.is_abstract && std::none_of(functions.begin(), functions.end(), [&](const auto &f) { return f.name == method.name; });
	}), obligations.trait_methods.end());
	const auto errors = gdscript::trait_conformance(script->get_metadata().functions, obligations, native_subtype);
	if (!errors.empty()) return fail(id, "Mod '" + id + "' " + String(manifest.get("version", "0")) + ": " + text(errors.front()));
	auto init = std::find_if(functions.begin(), functions.end(), [](const auto &f) { return f.name == "mod_init" && !f.is_static && f.parameters.size() == 1; });
	if (init == functions.end()) return fail(id, "Obligations must include mod_init(api)");
	Dictionary limits, requested = manifest.get("limits", Dictionary());
	for (size_t i = 0; i < 5; ++i) {
		int64_t ceiling = MAX(int64_t(1), int64_t(setting((String("sandbox/mods/limits/") + limit_names[i]).utf8().get_data(), defaults[i])));
		limits[limit_names[i]] = CLAMP(int64_t(requested.get(limit_names[i], ceiling)), int64_t(1), ceiling);
	}
	limits["binary_translation_nbit_as"] = manifest.get("binary_translation_nbit_as", true);
	Node *node = String(setting("sandbox/mods/node_type", "Node")) == "Node2D" ? static_cast<Node *>(memnew(Node2D)) : memnew(Node);
	node->set_name(id);
	Mod mod; mod.script = script; mod.node = node->get_instance_id(); mod.api = api->get_instance_id();
	for (const auto &method : offered.trait_methods) mod.methods.insert(method.name);
	mods.emplace(id, mod);
	script->configure_mod(limits, api, Callable(this, "_method_allowed").bind(id));
	node->set_script(script);
	Sandbox *vm = script->get_sandbox_for(node);
	mods[id].vm = vm;
	if (vm && (vm->get_exceptions() || vm->get_timeouts())) {
		guard.release();
		poll_faults(); return nullptr;
	}
	if (!vm || !vm->has_program_loaded()) {
		mods.erase(id); memdelete(node); return fail(id, "Mod program failed to initialize in its restricted Sandbox");
	}
	if (!vm->get_exceptions() && !vm->get_timeouts()) node->call("mod_init", api);
	if (vm->get_exceptions() || vm->get_timeouts()) {
		guard.release();
		poll_faults(); return nullptr;
	}
	callbacks(node, script, true);
	parent->add_child(node);
	set_process_internal(true);
	guard.release();
	poll_faults();
	if (is_quarantined(id) || !get_mod(id)) return nullptr;
	emit_signal("mod_loaded", id);
	return get_mod(id);
} catch (const std::exception &e) { return fail(manifest.get("id", ""), String("Mod load failed: ") + e.what()); }
catch (...) { return fail(manifest.get("id", ""), "Mod load failed"); }
Node *SgdModLoader::get_mod(const String &id) const {
	auto it = mods.find(id); return it == mods.end() ? nullptr : Object::cast_to<Node>(ObjectDB::get_instance(it->second.node));
}
bool SgdModLoader::is_quarantined(const String &id) const { auto it = mods.find(id); return it != mods.end() && it->second.quarantined; }
int64_t SgdModLoader::get_refusals(const String &id) const { auto it = mods.find(id); return it == mods.end() ? 0 : it->second.refusals; }
void SgdModLoader::poll_faults() noexcept try {
	// Snapshot IDs: signals may unload mods or start another load.
	std::vector<String> ids;
	for (const auto &entry : mods) ids.push_back(entry.first);
	for (const auto &id : ids) {
		auto it = mods.find(id);
		if (it == mods.end() || it->second.quarantined) continue;
		auto &mod = it->second;
		Node *node = get_mod(id);
		if (!node) { mods.erase(it); continue; }
		if (!mod.vm || (!mod.vm->get_exceptions() && !mod.vm->get_timeouts())) continue;
		mod.quarantined = true;
		const String reason = mod.vm->get_timeouts() ? "Instruction budget exhausted" : mod.vm->get_last_exception();
		const String location = mod.vm->get_last_exception_location();
		callbacks(node, mod.script, false);
		if (node->get_parent()) node->get_parent()->remove_child(node);
		last_error = reason;
		emit_signal("mod_faulted", id, reason, location);
	}
} catch (...) { ERR_PRINT("SgdModLoader: fault polling failed"); }
void SgdModLoader::unload(const String &id) noexcept try {
	if (busy) return;
	auto it = mods.find(id); if (it == mods.end()) return;
	Mod mod = it->second;
	Node *node = get_mod(id);
	busy = true;
	if (node) {
		if (!mod.quarantined) for (const auto &s : mod.script->get_metadata().functions) {
			if (s.name == "mod_deinit" && !s.is_static && s.parameters.empty()) { node->call("mod_deinit"); break; }
		}
		callbacks(node, mod.script, false);
		if (node->get_parent()) node->get_parent()->remove_child(node);
		memdelete(node);
	}
	mods.erase(id); busy = false;
} catch (...) { busy = false; ERR_PRINT("SgdModLoader: unload failed"); }
void SgdModLoader::_notification(int what) {
	if (what == NOTIFICATION_INTERNAL_PROCESS) poll_faults();
	if (what == NOTIFICATION_ENTER_TREE) {
		suspended = false;
		for (auto &entry : mods) {
			if (!entry.second.quarantined) if (Node *node = get_mod(entry.first)) callbacks(node, entry.second.script, true);
		}
	}
	if (what == NOTIFICATION_EXIT_TREE) {
		suspended = true;
		// The supplied parent may itself be propagating EXIT_TREE. Removing
		// siblings synchronously here corrupts its child traversal.
		for (auto &entry : mods) {
			if (Node *node = get_mod(entry.first)) callbacks(node, entry.second.script, false);
		}
	}
}
SgdModLoader::~SgdModLoader() {
	for (const auto &entry : mods) {
		Node *node = get_mod(entry.first);
		if (!node) continue;
		callbacks(node, entry.second.script, false);
		node->queue_free();
		if (node->get_parent()) node->get_parent()->remove_child(node);
	}
}
