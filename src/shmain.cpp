#include "riscv.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

int RiscvEmulator::test_static(int p_a, int p_b)
{
    return p_a + p_b;
}

void RiscvEmulator::test_static2()
{
    UtilityFunctions::print("  void static");
}

int RiscvEmulator::def_args(int p_a, int p_b)
{
    return p_a + p_b;
}

void RiscvEmulator::_notification(int p_what)
{
    //UtilityFunctions::print("Notification: ", String::num(p_what));
}

bool RiscvEmulator::_set(const StringName &p_name, const Variant &p_value)
{
    String name = p_name;
    if (name.begins_with("dproperty"))
    {
        int64_t index = name.get_slicec('_', 1).to_int();
        dprop[index] = p_value;
        return true;
    }
    if (name == "property_from_list")
    {
        property_from_list = p_value;
        return true;
    }
    return false;
}

bool RiscvEmulator::_get(const StringName &p_name, Variant &r_ret) const
{
    String name = p_name;
    if (name.begins_with("dproperty"))
    {
        int64_t index = name.get_slicec('_', 1).to_int();
        r_ret = dprop[index];
        return true;
    }
    if (name == "property_from_list")
    {
        r_ret = property_from_list;
        return true;
    }
    return false;
}

String RiscvEmulator::_to_string() const
{
    return "[ GDExtension::RiscvEmulator <--> Instance ID:" + uitos(get_instance_id()) + " ]";
}

void RiscvEmulator::_get_property_list(List<PropertyInfo> *p_list) const
{
    p_list->push_back(PropertyInfo(Variant::VECTOR3, "property_from_list"));
    for (int i = 0; i < 3; i++)
    {
        p_list->push_back(PropertyInfo(Variant::VECTOR2, "dproperty_" + itos(i)));
    }
}

bool RiscvEmulator::_property_can_revert(const StringName &p_name) const
{
    if (p_name == StringName("property_from_list") && property_from_list != Vector3(42, 42, 42))
    {
        return true;
    }
    else
    {
        return false;
    }
};

bool RiscvEmulator::_property_get_revert(const StringName &p_name, Variant &r_property) const
{
    if (p_name == StringName("property_from_list"))
    {
        r_property = Vector3(42, 42, 42);
        return true;
    }
    else
    {
        return false;
    }
};

void RiscvEmulator::_bind_methods()
{
    // Methods.
    ClassDB::bind_method(D_METHOD("simple_func"), &RiscvEmulator::simple_func);
    ClassDB::bind_method(D_METHOD("simple_const_func"), &RiscvEmulator::simple_const_func);
    ClassDB::bind_method(D_METHOD("return_something"), &RiscvEmulator::return_something);
    ClassDB::bind_method(D_METHOD("return_something_const"), &RiscvEmulator::return_something_const);

    ClassDB::bind_method(D_METHOD("test_array"), &RiscvEmulator::test_array);
    ClassDB::bind_method(D_METHOD("test_tarray_arg", "array"), &RiscvEmulator::test_tarray_arg);
    ClassDB::bind_method(D_METHOD("test_tarray"), &RiscvEmulator::test_tarray);
    ClassDB::bind_method(D_METHOD("test_dictionary"), &RiscvEmulator::test_dictionary);

    ClassDB::bind_method(D_METHOD("def_args", "a", "b"), &RiscvEmulator::def_args, DEFVAL(100), DEFVAL(200));

    ClassDB::bind_static_method("RiscvEmulator", D_METHOD("test_static", "a", "b"), &RiscvEmulator::test_static);
    ClassDB::bind_static_method("RiscvEmulator", D_METHOD("test_static2"), &RiscvEmulator::test_static2);

    {
        MethodInfo mi;
        mi.arguments.push_back(PropertyInfo(Variant::STRING, "some_argument"));
        mi.name = "varargs_func";
        ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "varargs_func", &RiscvEmulator::varargs_func, mi);
    }

    {
        MethodInfo mi;
        mi.arguments.push_back(PropertyInfo(Variant::STRING, "some_argument"));
        mi.name = "varargs_func_nv";
        ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "varargs_func_nv", &RiscvEmulator::varargs_func_nv, mi);
    }

    {
        MethodInfo mi;
        mi.arguments.push_back(PropertyInfo(Variant::STRING, "some_argument"));
        mi.name = "varargs_func_void";
        ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "varargs_func_void", &RiscvEmulator::varargs_func_void, mi);
    }

    // Properties.
    ADD_GROUP("Test group", "group_");
    ADD_SUBGROUP("Test subgroup", "group_subgroup_");

    ClassDB::bind_method(D_METHOD("get_custom_position"), &RiscvEmulator::get_custom_position);
    ClassDB::bind_method(D_METHOD("get_v4"), &RiscvEmulator::get_v4);
    ClassDB::bind_method(D_METHOD("set_custom_position", "position"), &RiscvEmulator::set_custom_position);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "group_subgroup_custom_position"), "set_custom_position", "get_custom_position");

    // Signals.
    ADD_SIGNAL(MethodInfo("custom_signal", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::INT, "value")));
    ClassDB::bind_method(D_METHOD("emit_custom_signal", "name", "value"), &RiscvEmulator::emit_custom_signal);

    // Constants.
    BIND_ENUM_CONSTANT(FIRST);
    BIND_ENUM_CONSTANT(ANSWER_TO_EVERYTHING);

    BIND_CONSTANT(CONSTANT_WITHOUT_ENUM);
}

RiscvEmulator::RiscvEmulator()
{
    UtilityFunctions::print("Constructor.");
}

RiscvEmulator::~RiscvEmulator()
{
    UtilityFunctions::print("Destructor.");
}

// Methods.
void RiscvEmulator::simple_func()
{
    UtilityFunctions::print("  Simple func called.");
}

void RiscvEmulator::simple_const_func() const
{
    UtilityFunctions::print("  Simple const func called.");
}

String RiscvEmulator::return_something(const String &base)
{
    UtilityFunctions::print("  Return something called.");
    return base;
}

Viewport *RiscvEmulator::return_something_const() const
{
    UtilityFunctions::print("  Return something const called.");
    if (is_inside_tree())
    {
        Viewport *result = get_viewport();
        return result;
    }
    return nullptr;
}

Variant RiscvEmulator::varargs_func(const Variant **args, GDNativeInt arg_count, GDNativeCallError &error)
{
    UtilityFunctions::print("  Varargs (Variant return) called with ", String::num((double)arg_count), " arguments");
    return arg_count;
}

int RiscvEmulator::varargs_func_nv(const Variant **args, GDNativeInt arg_count, GDNativeCallError &error)
{
    UtilityFunctions::print("  Varargs (int return) called with ", String::num((double)arg_count), " arguments");
    return 42;
}

void RiscvEmulator::varargs_func_void(const Variant **args, GDNativeInt arg_count, GDNativeCallError &error)
{
    UtilityFunctions::print("  Varargs (no return) called with ", String::num((double)arg_count), " arguments");
}

void RiscvEmulator::emit_custom_signal(const String &name, int value)
{
    emit_signal("custom_signal", name, value);
}

Array RiscvEmulator::test_array() const
{
    Array arr;

    arr.resize(2);
    arr[0] = Variant(1);
    arr[1] = Variant(2);

    return arr;
}

void RiscvEmulator::test_tarray_arg(const TypedArray<int64_t> &p_array)
{
    for (int i = 0; i < p_array.size(); i++)
    {
        UtilityFunctions::print(p_array[i]);
    }
}

TypedArray<Vector2> RiscvEmulator::test_tarray() const
{
    TypedArray<Vector2> arr;

    arr.resize(2);
    arr[0] = Vector2(1, 2);
    arr[1] = Vector2(2, 3);

    return arr;
}

Dictionary RiscvEmulator::test_dictionary() const
{
    Dictionary dict;

    dict["hello"] = "world";
    dict["foo"] = "bar";

    return dict;
}

// Properties.
void RiscvEmulator::set_custom_position(const Vector2 &pos)
{
    custom_position = pos;
}

Vector2 RiscvEmulator::get_custom_position() const
{
    return custom_position;
}

Vector4 RiscvEmulator::get_v4() const
{
    return Vector4(1.2, 3.4, 5.6, 7.8);
}
