mod godot;
use godot::variant::*;

pub fn main() {
}

#[no_mangle]
pub fn public_function() -> Variant {
	print1(&Variant::new_string("Hello from Rust!"));

	return Variant::new_float(3.14);
}
