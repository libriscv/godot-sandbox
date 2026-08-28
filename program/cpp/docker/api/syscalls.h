#pragma once
#define GAME_API_BASE 500

// System calls written in assembly
#define ECALL_PRINT (GAME_API_BASE + 0)
#define ECALL_VCALL (GAME_API_BASE + 1)
#define ECALL_VEVAL (GAME_API_BASE + 2)
#define ECALL_VASSIGN (GAME_API_BASE + 3)
#define ECALL_GET_OBJ (GAME_API_BASE + 4) // Get an object by name
#define ECALL_OBJ (GAME_API_BASE + 5) // All the Object functions
#define ECALL_OBJ_CALLP (GAME_API_BASE + 6) // Call a method on an object
#define ECALL_GET_NODE (GAME_API_BASE + 7) // Get a node by path
#define ECALL_NODE (GAME_API_BASE + 8) // All the Node functions
#define ECALL_NODE2D (GAME_API_BASE + 9) // All the Node2D functions
#define ECALL_NODE3D (GAME_API_BASE + 10) // All the Node3D functions

#define ECALL_THROW (GAME_API_BASE + 11)
#define ECALL_IS_EDITOR (GAME_API_BASE + 12)

#define ECALL_SINCOS (GAME_API_BASE + 13)
#define ECALL_VEC2_LENGTH (GAME_API_BASE + 14)
#define ECALL_VEC2_NORMALIZED (GAME_API_BASE + 15)
#define ECALL_VEC2_ROTATED (GAME_API_BASE + 16)

#define ECALL_VCREATE (GAME_API_BASE + 17)
#define ECALL_VCLONE (GAME_API_BASE + 18)
#define ECALL_VFETCH (GAME_API_BASE + 19)
#define ECALL_VSTORE (GAME_API_BASE + 20)

#define ECALL_ARRAY_OPS (GAME_API_BASE + 21)
#define ECALL_ARRAY_AT (GAME_API_BASE + 22)
#define ECALL_ARRAY_SIZE (GAME_API_BASE + 23)

#define ECALL_DICTIONARY_OPS (GAME_API_BASE + 24)

#define ECALL_STRING_CREATE (GAME_API_BASE + 25)
#define ECALL_STRING_OPS (GAME_API_BASE + 26)
#define ECALL_STRING_AT (GAME_API_BASE + 27)
#define ECALL_STRING_SIZE (GAME_API_BASE + 28)
#define ECALL_STRING_APPEND (GAME_API_BASE + 29)

#define ECALL_TIMER_PERIODIC (GAME_API_BASE + 30)
#define ECALL_TIMER_STOP (GAME_API_BASE + 31)

#define ECALL_NODE_CREATE (GAME_API_BASE + 32)

#define ECALL_MATH_OP32 (GAME_API_BASE + 33)
#define ECALL_MATH_OP64 (GAME_API_BASE + 34)
#define ECALL_LERP_OP32 (GAME_API_BASE + 35)
#define ECALL_LERP_OP64 (GAME_API_BASE + 36)

#define ECALL_VEC3_OPS (GAME_API_BASE + 37)

// callable_create(address, bound_variant, reserved, flags).
#define ECALL_CALLABLE_CREATE (GAME_API_BASE + 38)
// A3 flags. Bit 0: force Variant-pointer calling convention on this Callable.
#define ECALL_CALLABLE_VARIANT_ARGS 0x1

// load(path, len, result). len == ECALL_LOAD_PATH_IS_VARIANT: path is a Variant.
#define ECALL_LOAD (GAME_API_BASE + 39)
#define ECALL_LOAD_PATH_IS_VARIANT (~0ULL)

#define ECALL_TRANSFORM_2D_OPS (GAME_API_BASE + 40)
#define ECALL_TRANSFORM_3D_OPS (GAME_API_BASE + 41)
#define ECALL_BASIS_OPS (GAME_API_BASE + 42)

#define ECALL_VEC2_OPS (GAME_API_BASE + 43)

#define ECALL_QUAT_OPS (GAME_API_BASE + 44)

#define ECALL_OBJ_PROP_GET (GAME_API_BASE + 45)
#define ECALL_OBJ_PROP_SET (GAME_API_BASE + 46)

#define ECALL_SANDBOX_ADD (GAME_API_BASE + 47)
#define SANDBOX_ADD_PROPERTY 0
#define SANDBOX_ADD_METHOD 1
#define SANDBOX_ADD_EXIT_ADDRESS 2
#define SANDBOX_ADD_PROPERTY_HINT 3

#define ECALL_PACKED_ARRAY_OPS (GAME_API_BASE + 48)

// @GlobalScope's utility functions: str(), len() and the global math
// functions. See Utility_Op below.
#define ECALL_UTILITY (GAME_API_BASE + 49)

// Channelled print (printerr, prints, push_error, ...). See Print_Channel.
#define ECALL_PRINT_CHANNEL (GAME_API_BASE + 50)

// a0 = 1-based source line (cross-checked against line table + PC).
// Emitted only at requested lines; changing the set recompiles.
#define ECALL_BREAKPOINT (GAME_API_BASE + 51)

// a0 = operand GuestVariant*, a1 = frame base, a2 = frame size (bytes),
// a3 = state index, a4 = resume entry address, a5 = result slot offset (-1 = none).
// Returns a0 = 1 (suspended) or 0 (not awaitable; result slot already written).
#define ECALL_AWAIT (GAME_API_BASE + 52)

// a0 = frame base, a1 = frame size (checked against suspend).
// Returns a0 = state index. Result slot holds the awaited value.
#define ECALL_AWAIT_RESTORE (GAME_API_BASE + 53)

#define ECALL_CALL_GUEST (GAME_API_BASE + 54)

#define ECALL_VSCOPE (GAME_API_BASE + 55)

#define ECALL_OBJ_RETAIN (GAME_API_BASE + 56)

// Characters of a String in bulk, so walking one pays an ecall per batch and
// not per character. a0 = String variant index, a1 = index of the first
// character, a2 = how many at most. Returns (first scoped index << 32) | count:
// the characters occupy that many consecutive scoped slots. count is what was
// actually made -- short of the request near the end of the string, or when the
// reference budget leaves no room -- and 0 once the string is exhausted.
#define ECALL_STRING_BATCH (GAME_API_BASE + 57)

#define ECALL_VCONSTRUCT (GAME_API_BASE + 58)
#define ECALL_VSTORE_GLOBAL (GAME_API_BASE + 59)

// Attaches the nested class's script instance to the engine object the guest
// Dictionary holds under "@base", so Godot calls _ready/_process/_input on it.
// a0 = scoped index of the instance Dictionary, a1/a2 = class name pointer/length.
// No return value; throws when refused.
#define ECALL_CLASS_BIND (GAME_API_BASE + 60)

// ECALL_VCALL's arguments exactly, for a `super.method()` on a native base: the
// object's script instance would otherwise answer first and recurse into the
// lifted method that made the call.
#define ECALL_VCALL_SUPER (GAME_API_BASE + 61)

// get(subject, key, result): GDScript's `subject[key]`, delegated to Godot's
// Variant indexed-get dispatcher. All three arguments are GuestVariant pointers.
#define ECALL_VARIANT_GET (GAME_API_BASE + 62)

#define ECALL_LAST (GAME_API_BASE + 63)

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#define MAKE_SYSCALL(number, rval, name, ...)                      \
	__asm__(".pushsection .text\n"                                 \
			".global " #name "\n"                                  \
			".type " #name ", @function\n"                         \
			"" #name ":\n"                                         \
			"	li a7, " STRINGIFY(number) "\n"                    \
										   "   ecall\n"            \
										   "   ret\n"              \
										   ".popsection .text\n"); \
	extern "C" rval name(__VA_ARGS__);

#define EXTERN_SYSCALL(rval, name, ...) \
	extern "C" rval name(__VA_ARGS__);

enum class Scope_Op {
	MARK = 0,
	RELEASE = 1,
};

enum class Object_Op {
	GET_METHOD_LIST,
	GET,
	SET,
	GET_PROPERTY_LIST,
	CONNECT,
	DISCONNECT,
	GET_SIGNAL_LIST,
};

enum class Node_Create_Shortlist {
	CREATE_CLASSDB = 0,
	CREATE_NODE,
	CREATE_NODE2D,
	CREATE_NODE3D,
};

enum class Node_Op {
	GET_NAME = 0,
	GET_PATH,
	GET_PARENT,
	QUEUE_FREE,
	DUPLICATE,
	GET_CHILD_COUNT,
	GET_CHILD,
	ADD_CHILD,
	ADD_CHILD_DEFERRED,
	ADD_SIBLING,
	ADD_SIBLING_DEFERRED,
	MOVE_CHILD,
	REMOVE_CHILD,
	REMOVE_CHILD_DEFERRED,
	GET_CHILDREN,
	SET_NAME,
	REPARENT,
	REPLACE_BY,
	ADD_TO_GROUP,
	REMOVE_FROM_GROUP,
	IS_IN_GROUP,
	IS_INSIDE_TREE,
};

enum class Node2D_Op {
	GET_POSITION = 0,
	SET_POSITION,
	GET_ROTATION,
	SET_ROTATION,
	GET_SCALE,
	SET_SCALE,
	GET_SKEW,
	SET_SKEW,
	GET_TRANSFORM,
	SET_TRANSFORM,
};

enum class Node3D_Op {
	GET_POSITION = 0,
	SET_POSITION,
	GET_ROTATION,
	SET_ROTATION,
	GET_SCALE,
	SET_SCALE,
	GET_TRANSFORM,
	SET_TRANSFORM,
	GET_QUATERNION,
	SET_QUATERNION,
};

enum class Array_Op {
	CREATE = 0,
	PUSH_BACK,
	PUSH_FRONT,
	POP_AT,
	POP_BACK,
	POP_FRONT,
	INSERT,
	ERASE,
	RESIZE,
	CLEAR,
	SORT,
	FETCH_TO_VECTOR,
	HAS,
};

enum class Dictionary_Op {
	GET = 0,
	SET,
	ERASE,
	HAS,
	GET_KEYS,
	GET_VALUES,
	GET_SIZE,
	CLEAR,
	MERGE,
	GET_OR_ADD,
};

enum class String_Op {
	COPY = 0,
	GET_LENGTH,
	GET_CHAR,
	APPEND,
	INSERT,
	FIND,
	ERASE,
	TO_STD_STRING,
	COMPARE,
	COMPARE_CSTR,
};

enum class Math_Op {
	SIN = 0,
	COS,
	TAN,
	ASIN,
	ACOS,
	ATAN,
	ATAN2,
	POW,
};

enum class Lerp_Op {
	LERP = 0,
	SMOOTHSTEP,
	CLAMP,
	SLERP,
};

// -= GDScript's global functions =-
//
// @GlobalScope's utility functions -- str(), len(), and the arithmetic ones --
// as the guest asks the host to perform them, through ECALL_UTILITY.
//
// STR and LEN take a1 = the Variant to write the answer into, a2 = an array of
// argument Variants, a3 = how many. Every other op takes its arguments as
// doubles in fa0-fa7 and leaves a double in fa0; a predicate answers 0.0 or
// 1.0. The compiler mirrors these numbers in src/gdscript/compiler/globals.h,
// so an existing one may never change its meaning -- a new op goes on the end.
enum class Utility_Op {
	STR = 0,
	LEN = 1,

	// Unary: fa0
	FLOOR = 2,
	CEIL = 3,
	ROUND = 4,
	SIGN = 5,
	SIN = 6,
	COS = 7,
	TAN = 8,
	ASIN = 9,
	ACOS = 10,
	ATAN = 11,
	SINH = 12,
	COSH = 13,
	TANH = 14,
	ASINH = 15,
	ACOSH = 16,
	ATANH = 17,
	EXP = 18,
	LOG = 19,
	DEG_TO_RAD = 20,
	RAD_TO_DEG = 21,
	LINEAR_TO_DB = 22,
	DB_TO_LINEAR = 23,
	IS_NAN = 24,
	IS_INF = 25,
	IS_FINITE = 26,
	IS_ZERO_APPROX = 27,

	// Binary: fa0, fa1
	ATAN2 = 28,
	POW = 29,
	FMOD = 30,
	FPOSMOD = 31,
	SNAPPED = 32,
	IS_EQUAL_APPROX = 33,
	ANGLE_DIFFERENCE = 34,
	PINGPONG = 35,

	// Ternary: fa0, fa1, fa2
	LERP = 36,
	INVERSE_LERP = 37,
	SMOOTHSTEP = 38,
	MOVE_TOWARD = 39,
	LERP_ANGLE = 40,
	ROTATE_TOWARD = 41,
	WRAP = 42,

	// Five arguments: fa0 - fa4
	REMAP = 43,
	CUBIC_INTERPOLATE = 44,
	BEZIER_INTERPOLATE = 45,
	BEZIER_DERIVATIVE = 46,

	// Variant in, Variant out, the same shape as STR and LEN: a1 = the Variant
	// to write the answer into, a2 = the argument, a3 = 1. These are the type
	// constructors int(), float() and bool(), which convert anything a Variant
	// can hold -- a String among them -- and so cannot be arithmetic on fa0.
	TO_INT = 47,
	TO_FLOAT = 48,
	TO_BOOL = 49,

	// -= Randomness =-
	//
	// These draw from the same generator the rest of the project draws from,
	// so a call changes what the next call anywhere answers. That is the
	// reason they are the one family here that a caller may not skip, fold or
	// hoist.
	//
	// RANDF and friends are ordinary float ops: doubles in fa0-fa1, a double
	// out in fa0. RANDI and RANDI_RANGE instead take 64-bit integers in a1-a2
	// and answer in a0, because an int that does not fit a double must not
	// come back as a different int.
	RANDF = 50,
	RANDF_RANGE = 51,
	RANDFN = 52,
	RANDI = 53,
	RANDI_RANGE = 54,

	// fa0-fa4 arithmetic. step_decimals() fits in a double.
	EASE = 55,
	STEP_DECIMALS = 56,

	// int64 in a1, result in a0. Exceeds double range.
	NEAREST_PO2 = 57,

	// Variant in, Variant out. Same shape as STR/LEN.
	HASH = 58,
	VAR_TO_STR = 59,
	STR_TO_VAR = 60,
	VAR_TO_BYTES = 61,
	BYTES_TO_VAR = 62,
	TYPE_STRING = 63,
	TYPE_CONVERT = 64,
	ERROR_STRING = 65,
	IS_SAME = 66,

	CHAR = 67,
	ORD = 68,

	IS_INSTANCE_VALID = 69,

	// fa0-fa4. The angular form of CUBIC_INTERPOLATE.
	CUBIC_INTERPOLATE_ANGLE = 70,

	// Eight arguments: fa0-fa7. The Barry-Goldman forms take four extra
	// times alongside the four values, which is what widened the float
	// half of this call from fa0-fa4.
	CUBIC_INTERPOLATE_IN_TIME = 71,
	CUBIC_INTERPOLATE_ANGLE_IN_TIME = 72,

	// Variant in, Variant out. Same shape as STR/LEN.
	//
	// rand_from_seed() is the one random draw that is not a draw: it takes
	// the seed it uses and hands back the next one, leaving the project's
	// shared generator alone. That makes it pure, and safe under
	// restrictions, unlike randi() and friends.
	RAND_FROM_SEED = 73,

	// Variant in/out. These mutate the project's shared generator and are only
	// emitted by SafeGDScript compiled for an unrestricted Sandbox.
	RANDOMIZE = 74,
	SEED = 75,
};

// Output channel for ECALL_PRINT_CHANNEL.
enum class Print_Channel {
	PRINT = 0,          // print()
	SPACED = 1,         // prints()
	TABBED = 2,         // printt()
	RAW = 3,            // printraw(), no trailing newline
	RICH = 4,           // print_rich()
	ERROR = 5,          // printerr()
	VERBOSE = 6,        // print_verbose()
	PUSH_ERROR = 7,     // push_error()
	PUSH_WARNING = 8,   // push_warning()

	CHANNEL_COUNT
};

enum class Vec2_Op {
	NORMALIZE = 0,
	LENGTH,
	LENGTH_SQ,
	ANGLE,
	ANGLE_TO,
	ANGLE_TO_POINT,
	PROJECT,
	DIRECTION_TO,
	SLIDE,
	BOUNCE,
	REFLECT,
	LIMIT_LENGTH,
	LERP,
	CUBIC_INTERPOLATE,
	SLERP,
	MOVE_TOWARD,
	ROTATED,
};

enum class Vec3_Op {
	HASH = 0,
	LENGTH,
	NORMALIZE,
	DOT,
	CROSS,
	DISTANCE_TO,
	DISTANCE_SQ_TO,
	ANGLE_TO,
	PROJECT,
	REFLECT,
	ROTATED,
	FLOOR,
};

enum class Transform2D_Op {
	IDENTITY = 0,
	CREATE,
	ASSIGN,
	GET_COLUMN,
	SET_COLUMN,
	ROTATED,
	SCALED,
	TRANSLATED,
	INVERTED,
	AFFINE_INVERTED,
	ORTHONORMALIZED,
	LOOKING_AT,
	INTERPOLATE_WITH,
	XFORM,
	XFORM_INV,
};

enum class Transform3D_Op {
	IDENTITY = 0,
	CREATE,
	ASSIGN,
	GET_BASIS,
	SET_BASIS,
	GET_ORIGIN,
	SET_ORIGIN,
	ROTATED,
	ROTATED_LOCAL,
	SCALED,
	SCALED_LOCAL,
	TRANSLATED,
	TRANSLATED_LOCAL,
	INVERTED,
	AFFINE_INVERTED,
	ORTHONORMALIZED,
	LOOKING_AT,
	INTERPOLATE_WITH,
	XFORM,
	XFORM_INV,
};

enum class Basis_Op {
	IDENTITY = 0,
	CREATE,
	ASSIGN,
	GET_ROW,
	SET_ROW,
	GET_COLUMN,
	SET_COLUMN,
	INVERTED,
	TRANSPOSED,
	DETERMINANT,
	ROTATED,
	LERP,
	SLERP,
};

enum class Quaternion_Op {
	CREATE = 0,
	ASSIGN,
	DOT,
	LENGTH_SQUARED,
	LENGTH,
	NORMALIZE,
	INVERSE,
	LOG,
	EXP,
	ANGLE_TO,
	SLERP,
	SLERPNI,
	CUBIC_INTERPOLATE,
	CUBIC_INTERPOLATE_IN_TIME,
	AT,
	GET_AXIS,
	GET_ANGLE,
	MUL,
};
