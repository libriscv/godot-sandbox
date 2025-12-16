/**************************************************************************/
/*  gdscript.h                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "gdscript_function.h"

// Note: debugger headers may not be available in GDExtension
// #include "core/debugger/engine_debugger.h"
// #include "core/debugger/script_debugger.h"
// #include "core/doc_data.h"
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/classes/script_language_extension_profiling_info.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <gdextension_interface.h>

// ScriptInstanceExtension is defined in src/godot/script_instance.h
#include "../../godot/script_instance.h"

using namespace godot;

class GDScriptNativeClass : public RefCounted {
	GDCLASS(GDScriptNativeClass, RefCounted);

	StringName name;

protected:
	bool _get(const StringName &p_name, Variant &r_ret) const;
	static void _bind_methods();

public:
	_FORCE_INLINE_ const StringName &get_name() const { return name; }
	Variant _new();
	Object *instantiate();
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error);
	GDScriptNativeClass(const StringName &p_name);
};

class GDScript : public Script {
	GDCLASS(GDScript, Script);
	bool tool = false;
	bool valid = false;
	bool reloading = false;
	bool _is_abstract = false;

	struct MemberInfo {
		int index = 0;
		StringName setter;
		StringName getter;
		GDScriptDataType data_type;
		PropertyInfo property_info;
	};

	struct ClearData {
		HashSet<GDScriptFunction *> functions;
		HashSet<Ref<Script>> scripts;
		void clear() {
			functions.clear();
			scripts.clear();
		}
	};

	friend class GDScriptInstance;
	friend class GDScriptFunction;
	friend class GDScriptAnalyzer;
	friend class GDScriptCompiler;
	friend class GDScriptDocGen;
	friend class GDScriptLambdaCallable;
	friend class GDScriptLambdaSelfCallable;
	friend class GDScriptLanguage;
	friend struct GDScriptUtilityFunctionsDefinitions;
	friend class GDScriptELF; // For accessing MemberInfo

	Ref<GDScriptNativeClass> native;
	Ref<GDScript> base;
	GDScript *_base = nullptr; //fast pointer access

	GDScript *_script_owner = nullptr; //for subclasses (renamed to avoid collision with Wrapped::_owner)

private:
	// Members are just indices to the instantiated script.
	HashMap<StringName, MemberInfo> member_indices; // Includes member info of all base GDScript classes.
	HashSet<StringName> members; // Only members of the current class.

	// Only static variables of the current class.
	HashMap<StringName, MemberInfo> static_variables_indices;
	Vector<Variant> static_variables; // Static variable values.

	HashMap<StringName, Variant> constants;
	HashMap<StringName, GDScriptFunction *> member_functions;
	HashMap<StringName, Ref<GDScript>> subclasses;
	HashMap<StringName, MethodInfo> _signals;
	Dictionary rpc_config;

public:
	struct LambdaInfo {
		int capture_count;
		bool use_self;
	};

private:
	HashMap<GDScriptFunction *, LambdaInfo> lambda_info;

public:
	class UpdatableFuncPtr {
		friend class GDScript;

		GDScriptFunction *ptr = nullptr;
		GDScript *script = nullptr;
		List<UpdatableFuncPtr *>::Element *list_element = nullptr;

	public:
		GDScriptFunction *operator->() const { return ptr; }
		operator GDScriptFunction *() const { return ptr; }

		UpdatableFuncPtr(GDScriptFunction *p_function);
		~UpdatableFuncPtr();
	};

private:
	// List is used here because a ptr to elements are stored, so the memory locations need to be stable
	List<UpdatableFuncPtr *> func_ptrs_to_update;
	godot::Mutex func_ptrs_to_update_mutex;

	void _recurse_replace_function_ptrs(const HashMap<GDScriptFunction *, GDScriptFunction *> &p_replacements) const;

#ifdef TOOLS_ENABLED
	// For static data storage during hot-reloading.
	HashMap<StringName, MemberInfo> old_static_variables_indices;
	Vector<Variant> old_static_variables;
	void _save_old_static_data();
	void _restore_old_static_data();

	HashMap<StringName, int> member_lines;
	HashMap<StringName, Variant> member_default_values;
	List<PropertyInfo> members_cache;
	HashMap<StringName, Variant> member_default_values_cache;
	Ref<GDScript> base_cache;
	HashSet<ObjectID> inheriters_cache;
	bool source_changed_cache = false;
	bool placeholder_fallback_enabled = false;
	void _update_exports_values(HashMap<StringName, Variant> &values, List<PropertyInfo> &propnames);

	StringName doc_class_name;
	DocData::ClassDoc doc;
	Vector<DocData::ClassDoc> docs;
	void _add_doc(const DocData::ClassDoc &p_doc);
	void _clear_doc();
#endif

	GDScriptFunction *initializer = nullptr; // Direct pointer to `new()`/`_init()` member function, faster to locate.

	GDScriptFunction *implicit_initializer = nullptr; // `@implicit_new()` special function.
	GDScriptFunction *implicit_ready = nullptr; // `@implicit_ready()` special function.
	GDScriptFunction *static_initializer = nullptr; // `@static_initializer()` special function.

	Error _static_init();
	void _static_default_init(); // Initialize static variables with default values based on their types.

	int subclass_count = 0;
	HashSet<Object *> instances;
	bool destructing = false;
	bool clearing = false;
	//exported members
	String source;
	Vector<uint8_t> binary_tokens;
	String path;
	bool path_valid = false; // False if using default path.
	StringName local_name; // Inner class identifier or `class_name`.
	StringName global_name; // `class_name`.
	String fully_qualified_name;
	String simplified_icon_path;
	SelfList<GDScript> script_list;

	SelfList<GDScriptFunctionState>::List pending_func_states;

	GDScriptFunction *_super_constructor(GDScript *p_script);
	void _super_implicit_constructor(GDScript *p_script, GDScriptInstance *p_instance, GDExtensionCallError &r_error);
	GDScriptInstance *_create_instance(const Variant **p_args, int p_argcount, Object *p_owner, GDExtensionCallError &r_error);

	String _get_debug_path() const;

#ifdef TOOLS_ENABLED
	HashSet<void *> placeholders; // PlaceHolderScriptInstance not available in GDExtension
	//void _update_placeholder(PlaceHolderScriptInstance *p_placeholder);
	virtual void _placeholder_erased(void *p_placeholder); // PlaceHolderScriptInstance not available
	void _update_exports_down(bool p_base_exports_changed);
#endif

#ifdef DEBUG_ENABLED
	HashMap<ObjectID, List<Pair<StringName, Variant>>> pending_reload_state;
#endif

	bool _update_exports(bool *r_err = nullptr, bool p_recursive_call = false, void *p_instance_to_update = nullptr, bool p_base_exports_changed = false); // PlaceHolderScriptInstance not available

	void _save_orphaned_subclasses(GDScript::ClearData *p_clear_data);

	void _get_script_property_list(List<PropertyInfo> *r_list, bool p_include_base) const;
	void _get_script_method_list(List<MethodInfo> *r_list, bool p_include_base) const;
	void _get_script_signal_list(List<MethodInfo> *r_list, bool p_include_base) const;

	GDScript *_get_gdscript_from_variant(const Variant &p_variant);
	void _collect_function_dependencies(GDScriptFunction *p_func, HashSet<GDScript *> &p_dependencies, const GDScript *p_except);
	void _collect_dependencies(HashSet<GDScript *> &p_dependencies, const GDScript *p_except);

protected:
	bool _get(const StringName &p_name, Variant &r_ret) const;
	bool _set(const StringName &p_name, const Variant &p_value);
	void _get_property_list(List<PropertyInfo> *p_properties) const;

	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error);

	static void _bind_methods();

public:
#ifdef DEBUG_ENABLED
	static String debug_get_script_name(const Ref<Script> &p_script);
#endif

	static String canonicalize_path(const String &p_path);
	_FORCE_INLINE_ static bool is_canonically_equal_paths(const String &p_path_a, const String &p_path_b) {
		return canonicalize_path(p_path_a) == canonicalize_path(p_path_b);
	}

	_FORCE_INLINE_ StringName get_local_name() const { return local_name; }

	void clear(GDScript::ClearData *p_clear_data = nullptr);

	// Cancels all functions of the script that are are waiting to be resumed after using await.
	void cancel_pending_functions(bool warn);

	bool is_valid() const { return valid; }

	bool inherits_script(const Ref<Script> &p_script) const;

	GDScript *find_class(const String &p_qualified_name);
	bool has_class(const GDScript *p_script);
	GDScript *get_root_script();
	bool is_root_script() const { return _script_owner == nullptr; }
	String get_fully_qualified_name() const { return fully_qualified_name; }
	const HashMap<StringName, Ref<GDScript>> &get_subclasses() const { return subclasses; }
	const HashMap<StringName, Variant> &get_constants() const { return constants; }
	const HashSet<StringName> &get_members() const { return members; }
	const GDScriptDataType &get_member_type(const StringName &p_member) const {
		CRASH_COND(!member_indices.has(p_member));
		return member_indices[p_member].data_type;
	}
	const Ref<GDScriptNativeClass> &get_native() const { return native; }

	_FORCE_INLINE_ const HashMap<StringName, GDScriptFunction *> &get_member_functions() const { return member_functions; }
	_FORCE_INLINE_ const HashMap<GDScriptFunction *, LambdaInfo> &get_lambda_info() const { return lambda_info; }

	_FORCE_INLINE_ const GDScriptFunction *get_implicit_initializer() const { return implicit_initializer; }
	_FORCE_INLINE_ const GDScriptFunction *get_implicit_ready() const { return implicit_ready; }
	_FORCE_INLINE_ const GDScriptFunction *get_static_initializer() const { return static_initializer; }

	HashSet<GDScript *> get_dependencies();
	HashMap<GDScript *, HashSet<GDScript *>> get_all_dependencies();
	HashSet<GDScript *> get_must_clear_dependencies();

	bool has_script_signal(const StringName &p_signal) const;
	void get_script_signal_list(List<MethodInfo> *r_signals) const;

	bool is_tool() const { return tool; }
	bool is_abstract() const { return _is_abstract; }
	Ref<GDScript> get_base() const;

	const HashMap<StringName, MemberInfo> &debug_get_member_indices() const { return member_indices; }
	const HashMap<StringName, GDScriptFunction *> &debug_get_member_functions() const; //this is debug only
	StringName debug_get_member_by_index(int p_idx) const;
	StringName debug_get_static_var_by_index(int p_idx) const;

	Variant _new(const Variant **p_args, int p_argcount, GDExtensionCallError &r_error);
	bool can_instantiate() const;

	Ref<Script> get_base_script() const;
	StringName get_global_name() const;

	StringName get_instance_base_type() const; // this may not work in all scripts, will return empty if so
	void *instance_create(Object *p_this); // ScriptInstanceExtension not directly available
	void *placeholder_instance_create(Object *p_this); // PlaceHolderScriptInstance not available in GDExtension
	bool instance_has(const Object *p_this) const;

	bool has_source_code() const;
	String get_source_code() const;
	void set_source_code(const String &p_code);
	void update_exports();

#ifdef TOOLS_ENABLED
	virtual StringName get_doc_class_name() const { return doc_class_name; }
	virtual Vector<DocData::ClassDoc> get_documentation() const { return docs; }
	virtual String get_class_icon_path() const;
#endif // TOOLS_ENABLED

	Error reload(bool p_keep_state = false);

	void set_path(const String &p_path, bool p_take_over = false);
	String get_script_path() const;
	Error load_source_code(const String &p_path);

	void set_binary_tokens_source(const Vector<uint8_t> &p_binary_tokens);
	const Vector<uint8_t> &get_binary_tokens_source() const;
	Vector<uint8_t> get_as_binary_tokens() const;

	bool get_property_default_value(const StringName &p_property, Variant &r_value) const;

	void get_script_method_list(List<MethodInfo> *p_list) const;
	bool has_method(const StringName &p_method) const;
	bool has_static_method(const StringName &p_method) const;

	int get_script_method_argument_count(const StringName &p_method, bool *r_is_valid = nullptr) const;

	MethodInfo get_method_info(const StringName &p_method) const;

	void get_script_property_list(List<PropertyInfo> *p_list) const;

	ScriptLanguageExtension *get_language() const;

	int get_member_line(const StringName &p_member) const {
#ifdef TOOLS_ENABLED
		if (member_lines.has(p_member)) {
			return member_lines[p_member];
		}
#endif
		return -1;
	}

	virtual void get_constants(HashMap<StringName, Variant> *p_constants);
	virtual void get_members(HashSet<StringName> *p_members);

	virtual const Variant get_rpc_config() const;

	void unload_static() const;

#ifdef TOOLS_ENABLED
	virtual bool is_placeholder_fallback_enabled() const { return placeholder_fallback_enabled; }
#endif

	GDScript();
	~GDScript();
};

class GDScriptInstance : public ScriptInstanceExtension {
	friend class GDScript;
	friend class GDScriptFunction;
	friend class GDScriptLambdaCallable;
	friend class GDScriptLambdaSelfCallable;
	friend class GDScriptCompiler;
	friend class GDScriptCache;
	friend struct GDScriptUtilityFunctionsDefinitions;

	ObjectID owner_id;
	Object *owner = nullptr;
	Ref<GDScript> script;
#ifdef DEBUG_ENABLED
	HashMap<StringName, int> member_indices_cache; //used only for hot script reloading
#endif
	Vector<Variant> members;

	SelfList<GDScriptFunctionState>::List pending_func_states;

	void _call_implicit_ready_recursively(GDScript *p_script);

public:
	virtual Object *get_owner() override { return owner; }

	virtual bool set(const StringName &p_name, const Variant &p_value) override;
	virtual bool get(const StringName &p_name, Variant &r_ret) const override;
	virtual const GDExtensionPropertyInfo *get_property_list(uint32_t *r_count) const override;
	virtual void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) const override;
	virtual Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid = nullptr) const override;
	virtual bool validate_property(GDExtensionPropertyInfo &p_property) const override;

	virtual bool property_can_revert(const StringName &p_name) const override;
	virtual bool property_get_revert(const StringName &p_name, Variant &r_ret) const override;

	virtual const GDExtensionMethodInfo *get_method_list(uint32_t *r_count) const override;
	virtual void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) const override;
	virtual bool has_method(const StringName &p_method) const override;

	virtual GDExtensionInt get_method_argument_count(const StringName &p_method, bool &r_valid) const override;

	virtual Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, GDExtensionCallError &r_error) override;

	virtual void refcount_incremented() override;
	virtual bool refcount_decremented() override;
	virtual bool is_placeholder() const override;
	virtual void property_set_fallback(const StringName &p_name, const Variant &p_value, bool *r_valid) override;
	virtual Variant property_get_fallback(const StringName &p_name, bool *r_valid) override;
	virtual void get_property_state(GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) override;

	Variant debug_get_member_by_index(int p_idx) const { return members[p_idx]; }

	virtual void notification(int p_notification, bool p_reversed = false) override;
	virtual String to_string(bool *r_valid) override;

	virtual Ref<Script> get_script() const override;

	virtual ScriptLanguage *_get_language() override;

	void set_path(const String &p_path);

	void reload_members();

	virtual const Variant get_rpc_config() const;

	GDScriptInstance() {}
	~GDScriptInstance();
};

class GDScriptLanguage : public ScriptLanguage {
	friend class GDScriptFunctionState;

	static GDScriptLanguage *singleton;

	bool finishing = false;

	Variant *_global_array = nullptr;
	Vector<Variant> global_array;
	HashMap<StringName, int> globals;
	HashMap<StringName, Variant> named_globals;
	Vector<int> global_array_empty_indexes;

	struct CallLevel {
		Variant *stack = nullptr;
		GDScriptFunction *function = nullptr;
		GDScriptInstance *instance = nullptr;
		int *ip = nullptr;
		int *line = nullptr;
		CallLevel *prev = nullptr; // Reverse linked list (stack).
	};

	static thread_local int _debug_parse_err_line;
	static thread_local String _debug_parse_err_file;
	static thread_local String _debug_error;

	static thread_local CallLevel *_call_stack;
	static thread_local uint32_t _call_stack_size;
	uint32_t _debug_max_call_stack = 0;

	bool track_call_stack = false;
	bool track_locals = false;

	static CallLevel *_get_stack_level(uint32_t p_level);

	void _add_global(const StringName &p_name, const Variant &p_value);
	void _remove_global(const StringName &p_name);

	friend class GDScriptInstance;

	Mutex mutex;

	friend class GDScript;

	SelfList<GDScript>::List script_list;
	friend class GDScriptFunction;

	SelfList<GDScriptFunction>::List function_list;
#ifdef DEBUG_ENABLED
	bool profiling;
	bool profile_native_calls;
	uint64_t script_frame_time;
#endif

	HashMap<String, ObjectID> orphan_subclasses;

#ifdef TOOLS_ENABLED
	void _extension_loaded(const Ref<GDExtension> &p_extension);
	void _extension_unloading(const Ref<GDExtension> &p_extension);
#endif

public:
	bool debug_break(const String &p_error, bool p_allow_continue = true);
	bool debug_break_parse(const String &p_file, int p_line, const String &p_error);

	_FORCE_INLINE_ void enter_function(CallLevel *call_level, GDScriptInstance *p_instance, GDScriptFunction *p_function, Variant *p_stack, int *p_ip, int *p_line) {
		if (!track_call_stack) {
			return;
		}

#ifdef DEBUG_ENABLED
		ScriptDebugger *script_debugger = EngineDebugger::get_script_debugger();
		if (script_debugger != nullptr && script_debugger->get_lines_left() > 0 && script_debugger->get_depth() >= 0) {
			script_debugger->set_depth(script_debugger->get_depth() + 1);
		}
#endif

		if (unlikely(_call_stack_size >= _debug_max_call_stack)) {
			_debug_error = vformat("Stack overflow (stack size: %s). Check for infinite recursion in your script.", _debug_max_call_stack);

#ifdef DEBUG_ENABLED
			if (script_debugger != nullptr) {
				script_debugger->debug(this);
			}
#endif

			return;
		}

		call_level->prev = _call_stack;
		_call_stack = call_level;
		call_level->stack = p_stack;
		call_level->instance = p_instance;
		call_level->function = p_function;
		call_level->ip = p_ip;
		call_level->line = p_line;
		_call_stack_size++;
	}

	_FORCE_INLINE_ void exit_function() {
		if (!track_call_stack) {
			return;
		}

#ifdef DEBUG_ENABLED
		ScriptDebugger *script_debugger = EngineDebugger::get_script_debugger();
		if (script_debugger && script_debugger->get_lines_left() > 0 && script_debugger->get_depth() >= 0) {
			script_debugger->set_depth(script_debugger->get_depth() - 1);
		}
#endif

		if (unlikely(_call_stack_size == 0)) {
#ifdef DEBUG_ENABLED
			if (script_debugger) {
				_debug_error = "Stack Underflow (Engine Bug)";
				script_debugger->debug(this);
			} else {
				ERR_PRINT("Stack underflow! (Engine Bug)");
			}
#else // !DEBUG_ENABLED
			ERR_PRINT("Stack underflow! (Engine Bug)");
#endif
			return;
		}

		_call_stack_size--;
		_call_stack = _call_stack->prev;
	}

	godot::TypedArray<godot::Dictionary> debug_get_current_stack_info() {
		godot::TypedArray<godot::Dictionary> csi;
		csi.resize(_call_stack_size);
		CallLevel *cl = _call_stack;
		uint32_t idx = 0;
		while (cl) {
			Dictionary stack_info;
			stack_info["line"] = *cl->line;
			if (cl->function) {
				stack_info["func"] = cl->function->get_name();
				stack_info["file"] = cl->function->get_script()->get_script_path();
			}
			csi[idx] = stack_info;
			idx++;
			cl = cl->prev;
		}
		return csi;
	}

	struct {
		StringName _init;
		StringName _static_init;
		StringName _notification;
		StringName _set;
		StringName _get;
		StringName _get_property_list;
		StringName _validate_property;
		StringName _property_can_revert;
		StringName _property_get_revert;
		StringName _script_source;

	} strings;

	_FORCE_INLINE_ bool should_track_call_stack() const { return track_call_stack; }
	_FORCE_INLINE_ bool should_track_locals() const { return track_locals; }
	_FORCE_INLINE_ int get_global_array_size() const { return global_array.size(); }
	_FORCE_INLINE_ Variant *get_global_array() { return _global_array; }
	_FORCE_INLINE_ const HashMap<StringName, int> &get_global_map() const { return globals; }
	_FORCE_INLINE_ const HashMap<StringName, Variant> &get_named_globals_map() const { return named_globals; }
	// These two functions should be used when behavior needs to be consistent between in-editor and running the scene
	bool has_any_global_constant(const StringName &p_name) { return named_globals.has(p_name) || globals.has(p_name); }
	Variant get_any_global_constant(const StringName &p_name);

	_FORCE_INLINE_ static GDScriptLanguage *get_singleton() { return singleton; }

	virtual String get_name() const;

	/* LANGUAGE FUNCTIONS */
	virtual void init();
	virtual String get_type() const;
	virtual String get_extension() const;
	virtual void finish();

	/* EDITOR FUNCTIONS */
	virtual Vector<String> get_reserved_words() const;
	virtual bool is_control_flow_keyword(const String &p_keywords) const;
	virtual Vector<String> get_comment_delimiters() const;
	virtual Vector<String> get_doc_comment_delimiters() const;
	virtual Vector<String> get_string_delimiters() const;
	virtual bool is_using_templates();
	virtual Ref<Script> make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const;
	// ScriptTemplate doesn't exist in GDExtension - use Dictionary instead
	virtual godot::TypedArray<godot::Dictionary> get_built_in_templates(const StringName &p_object);
	// ScriptError and Warning don't exist in GDExtension - return Dictionary instead
	virtual Dictionary validate(const String &p_script, const String &p_path = "", bool p_validate_functions = true, bool p_validate_errors = true, bool p_validate_warnings = true, bool p_validate_safe_lines = true) const;
	virtual Script *create_script() const;
	virtual bool supports_builtin_mode() const;
	virtual bool supports_documentation() const;
	virtual bool can_inherit_from_file() const { return true; }
	virtual int find_function(const String &p_function, const String &p_code) const;
	virtual String make_function(const String &p_class, const String &p_name, const PackedStringArray &p_args) const;
	// CodeCompletionOption doesn't exist in GDExtension - return Dictionary instead
	virtual Dictionary complete_code(const String &p_code, const String &p_path, Object *p_owner);
#ifdef TOOLS_ENABLED
	virtual Error lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner, LookupResult &r_result);
#endif
	virtual String _get_indentation() const;
	virtual void auto_indent_code(String &p_code, int p_from_line, int p_to_line) const;
	virtual void add_global_constant(const StringName &p_variable, const Variant &p_value);
	virtual void add_named_global_constant(const StringName &p_name, const Variant &p_value);
	virtual void remove_named_global_constant(const StringName &p_name);

	/* DEBUGGER FUNCTIONS */

	virtual String debug_get_error() const;
	virtual int debug_get_stack_level_count() const;
	virtual int debug_get_stack_level_line(int p_level) const;
	virtual String debug_get_stack_level_function(int p_level) const;
	virtual String debug_get_stack_level_source(int p_level) const;
	virtual void debug_get_stack_level_locals(int p_level, List<String> *p_locals, List<Variant> *p_values, int p_max_subitems = -1, int p_max_depth = -1);
	virtual void debug_get_stack_level_members(int p_level, List<String> *p_members, List<Variant> *p_values, int p_max_subitems = -1, int p_max_depth = -1);
	// ScriptInstance doesn't exist in GDExtension - use ScriptInstanceExtension
	virtual void *debug_get_stack_level_instance(int p_level);
	virtual void debug_get_globals(List<String> *p_globals, List<Variant> *p_values, int p_max_subitems = -1, int p_max_depth = -1);
	virtual String debug_parse_stack_level_expression(int p_level, const String &p_expression, int p_max_subitems = -1, int p_max_depth = -1);

	virtual void reload_all_scripts();
	virtual void reload_scripts(const Array &p_scripts, bool p_soft_reload);
	virtual void reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload);

	virtual void frame();

	virtual void get_public_functions(List<MethodInfo> *p_functions) const;
	virtual void get_public_constants(List<Pair<String, Variant>> *p_constants) const;
	virtual void get_public_annotations(List<MethodInfo> *p_annotations) const;

	virtual void profiling_start();
	virtual void profiling_stop();
	virtual void profiling_set_save_native_calls(bool p_enable);
	void profiling_collate_native_call_data(bool p_accumulated);

	// ProfilingInfo doesn't exist in GDExtension - use ScriptLanguageExtensionProfilingInfo
	virtual int profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_arr, int p_info_max);
	virtual int profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_arr, int p_info_max);

	/* LOADER FUNCTIONS */

	virtual void get_recognized_extensions(List<String> *p_extensions) const;

	/* GLOBAL CLASSES */

	virtual bool handles_global_class_type(const String &p_type) const;
	virtual String get_global_class_name(const String &p_path, String *r_base_type = nullptr, String *r_icon_path = nullptr, bool *r_is_abstract = nullptr, bool *r_is_tool = nullptr) const;

	void add_orphan_subclass(const String &p_qualified_name, const ObjectID &p_subclass);
	Ref<GDScript> get_orphan_subclass(const String &p_qualified_name);

	Ref<GDScript> get_script_by_fully_qualified_name(const String &p_name);

	GDScriptLanguage();
	~GDScriptLanguage();
};

class ResourceFormatLoaderGDScript : public ResourceFormatLoader {
	GDCLASS(ResourceFormatLoaderGDScript, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, ResourceLoader::CacheMode p_cache_mode = ResourceLoader::CACHE_MODE_REUSE);
	virtual void get_recognized_extensions(List<String> *p_extensions) const;
	virtual bool handles_type(const String &p_type) const;
	virtual String get_resource_type(const String &p_path) const;
	virtual void get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types = false);
	virtual void get_classes_used(const String &p_path, HashSet<StringName> *r_classes);
};

class ResourceFormatSaverGDScript : public ResourceFormatSaver {
	GDCLASS(ResourceFormatSaverGDScript, ResourceFormatSaver);

public:
	virtual Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0);
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const;
	virtual bool recognize(const Ref<Resource> &p_resource) const;
};
