#![allow(dead_code)]
use std::arch::asm;
use std::arch::global_asm;

pub struct Engine
{
}

impl Engine
{
	pub fn is_editor_hint() -> bool
	{
		let is_editor: i32;
		unsafe {
			asm!("ecall",
				in("a7") 512, // ECALL_IS_EDITOR
				lateout("a0") is_editor,
				options(nostack));
		}
		return is_editor != 0;
	}
}

// Godot Rust API version embedded in the binary
global_asm!(
	".pushsection .comment",
	".string \"Godot Rust API v4\"",
	".popsection",
);
