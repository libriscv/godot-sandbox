use super::syscalls::*;

#[repr(C)]
pub struct GodotString {
	pub reference: i32
}

impl GodotString {
	pub fn new() -> GodotString {
		GodotString {
			reference: i32::MIN
		}
	}

	pub fn create(s: &str) -> GodotString {
		let mut godot_string = GodotString::new();
		godot_string.reference = godot_string_create(s);
		godot_string
	}

	pub fn from_ref(var_ref: i32) -> GodotString {
		GodotString {
			reference: var_ref
		}
	}

	pub fn to_string(&self) -> String {
		godot_string_to_string(self.reference)
	}
}
