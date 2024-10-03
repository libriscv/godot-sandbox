#include <api.hpp>
#include <cstring>
extern "C" {
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <luajit-2.1/lualib.h>
}
extern "C" Variant click();

static int api_print(lua_State *L) {
	const char *text = luaL_checkstring(L, 1);
	get_node("../TextAnswer")("set_text", text);
	return 0;
}

lua_State *L;
int main() {
	// Activate this mod
	get_parent().call("set_visible", true);
	get_node("../Button").connect("pressed", Callable(click));

	Timer::native_periodic(0.0125, [](Node timer) -> Variant {
		Node2D mod = get_parent(); // From the Timers POV
		static Vector2 origin = mod.get_position();
		static constexpr float period = 2.0f;
		static float x = 0.0f;
		const float progress = 1.0f - x / 4.0f;
		if (progress <= 0.0f) {
			timer.queue_free();
		}

		const float anim = (Math::sin(x * period + x) * 2.0f - 1.0f) * 0.1f * progress;
		const Vector2 scale = Vector2::From(1.0f + anim);
		mod.set_position(origin - scale * 55.0f);
		mod.set_scale(scale);
		x += 0.1f;
		return Nil;
	});

	L = luaL_newstate();

	luaL_openlibs(L); /* Load Lua libraries */

	// API bindings
	lua_register(L, "print", api_print);

	// Load a string as a script
	luaL_loadstring(L, "print('Hello, Lua!')");

	// Run the script
	lua_pcall(L, 0, 0, 0);

	fflush(stdout);

	halt();
}

extern "C" Variant run(String code) {
	// Load a string as a script
	const std::string utf = code.utf8();
	luaL_loadbuffer(L, utf.c_str(), utf.size(), "@code");

	// Run the script
	lua_pcall(L, 0, 0, 0);

	fflush(stdout);
	return Nil;
}

extern "C" Variant click() {
	String text = get_node("../TextEdit")("get_text");
	return run(text);
}
