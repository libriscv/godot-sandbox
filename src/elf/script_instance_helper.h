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

static void add_to_state(GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value, void *p_userdata) {
	List<Pair<StringName, Variant>> *list = reinterpret_cast<List<Pair<StringName, Variant>> *>(p_userdata);
	list->push_back({ *(const StringName *)p_name, *(const Variant *)p_value });
}
