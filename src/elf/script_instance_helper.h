#include "../register_types.h"
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/templates/pair.hpp>

using namespace godot;

static int get_len_from_ptr(const void *p_ptr) {
	return *((int *)p_ptr - 1);
}

static void free_with_len(void *p_ptr) {
	memfree((int *)p_ptr - 1);
}

static void free_prop(const GDExtensionPropertyInfo &p_prop) {
	// smelly
	memdelete((StringName *)p_prop.name);
	memdelete((StringName *)p_prop.class_name);
	memdelete((String *)p_prop.hint_string);
}

static String *string_alloc(const String &p_str) {
	String *ptr = memnew(String);
	*ptr = p_str;

	return ptr;
}

static StringName *stringname_alloc(const String &p_str) {
	StringName *ptr = memnew(StringName);
	*ptr = p_str;

	return ptr;
}

static GDExtensionPropertyInfo create_property_type(const Dictionary &p_src) {
	GDExtensionPropertyInfo p_dst;
	p_dst.type = (GDExtensionVariantType) int(p_src["type"]);
	p_dst.name = stringname_alloc(p_src["name"]);
	p_dst.class_name = stringname_alloc(p_src["class_name"]);
	p_dst.hint = p_src["hint"];
	p_dst.hint_string = string_alloc(p_src["hint_string"]);
	p_dst.usage = p_src["usage"];
	return p_dst;
}

static GDExtensionMethodInfo create_method_info(const MethodInfo &method_info) {
	GDExtensionMethodInfo result{
	   .name = stringname_alloc(method_info.name),
	   .return_value = GDExtensionPropertyInfo{
			   .type = (GDExtensionVariantType)method_info.return_val.type,
			   .name = stringname_alloc(method_info.return_val.name),
			   .class_name = stringname_alloc(method_info.return_val.class_name),
			   .hint = method_info.return_val.hint,
			   .hint_string = string_alloc(method_info.return_val.hint_string),
			   .usage = method_info.return_val.usage },
	   .flags = method_info.flags,
	   .id = method_info.id,
	   .argument_count = (uint32_t)method_info.arguments.size(),
	   .arguments = nullptr,
	   .default_argument_count = (uint32_t)method_info.default_arguments.size(),
	   .default_arguments = nullptr,
   };
   if (!method_info.arguments.is_empty()) {
	   result.arguments = memnew_arr(GDExtensionPropertyInfo, method_info.arguments.size());
	   for (int i = 0; i < method_info.arguments.size(); i++) {
		   const PropertyInfo &arg = method_info.arguments[i];
		   result.arguments[i] = GDExtensionPropertyInfo{
				   .type = (GDExtensionVariantType)arg.type,
				   .name = stringname_alloc(arg.name),
				   .class_name = stringname_alloc(arg.class_name),
				   .hint = arg.hint,
				   .hint_string = stringname_alloc(arg.hint_string),
				   .usage = arg.usage };
	   }
   }
   // The defaults cover the last N arguments, as they do in Godot's own method
   // lists. Despite the GDExtensionVariantPtr* in the struct, the engine reads
   // this back as one contiguous array of Variants -- an array of pointers is
   // read as Variants and crashes on the first type tag. free_method_info()
   // owns the array.
   if (!method_info.default_arguments.is_empty()) {
	   Variant *defaults = memnew_arr(Variant, method_info.default_arguments.size());
	   for (int i = 0; i < method_info.default_arguments.size(); i++) {
		   defaults[i] = method_info.default_arguments[i];
	   }
	   result.default_arguments = (GDExtensionVariantPtr *)defaults;
   }
   return result;
}

// Undo create_method_info(). Both script languages' free_method_list() go
// through this, so anything allocated above has exactly one place to be
// released.
static void free_method_info(const GDExtensionMethodInfo &p_method_info) {
	memdelete((StringName *)p_method_info.name);
	free_prop(p_method_info.return_value);
	if (p_method_info.arguments) {
		for (uint32_t i = 0; i < p_method_info.argument_count; i++) {
			free_prop(p_method_info.arguments[i]);
		}
		memdelete_arr(p_method_info.arguments);
	}
	if (p_method_info.default_arguments) {
		memdelete_arr((Variant *)p_method_info.default_arguments);
	}
}

static void add_to_state(GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value, void *p_userdata) {
	List<Pair<StringName, Variant>> *list = reinterpret_cast<List<Pair<StringName, Variant>> *>(p_userdata);
	list->push_back({ *(const StringName *)p_name, *(const Variant *)p_value });
}

static Dictionary prop_to_dict(const PropertyInfo &p_prop) {
	Dictionary d;
	d["name"] = p_prop.name;
	d["type"] = p_prop.type;
	d["class_name"] = p_prop.class_name;
	d["hint"] = p_prop.hint;
	d["hint_string"] = p_prop.hint_string;
	d["usage"] = p_prop.usage;
	return d;
}

static Dictionary method_to_dict(const MethodInfo &p_method) {
	Dictionary d;

	d["name"] = p_method.name;
	d["flags"] = p_method.flags;

	if (p_method.arguments.size() > 0) {
		Array args;
		for (const PropertyInfo &arg : p_method.arguments) {
			args.push_back(prop_to_dict(arg));
		}
		d["args"] = args;
	}

	if (p_method.default_arguments.size() > 0) {
		Array defaults;
		for (const Variant &value : p_method.default_arguments) {
			defaults.push_back(value);
		}
		d["default_args"] = defaults;
	}

	d["return"] = prop_to_dict(p_method.return_val);

	return d;
}
