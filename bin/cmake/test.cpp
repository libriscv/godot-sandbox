#include "api.hpp"

PUBLIC Variant public_function(String arg) {
	print("Arguments: ", arg);
	return "Hello from the other side";
}

PUBLIC Variant _ready() {
	public_function("test");
	return Nil;
}

PUBLIC Variant _process() {
	public_function("test1");
	return Nil;
}
