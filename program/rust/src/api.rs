use std::arch::asm;
const VARIANT_SIZE: usize = 24;

#[repr(C)]
pub enum VariantType {
	Nil,
	Bool,
	Integer,
	Float,
	String,
}

#[repr(C)]
pub(self) union CapacityOrSSO {
	pub cap: usize,
	pub sso: [u8; 16],
}

#[repr(C)]
pub struct VariantStdString {
	// 64-bit pointer + 64-bit length + 64-bit capacity OR 16-byte SSO data
	pub ptr: *const char,
	pub len: usize,
	pub(self) cap_or_sso: CapacityOrSSO,
}

#[repr(C)]
pub union VariantUnion {
	pub b: bool,
	pub i: i32,
	pub f: f64,
	pub s: *const VariantStdString,
	// 24 bytes total
	pub a: [u8; VARIANT_SIZE],
}

#[repr(C)]
pub struct Variant
{
	pub t : VariantType,
	pub u : VariantUnion,
}

impl Variant
{
	pub fn new_nil() -> Variant
	{
		let v = Variant { t: VariantType::Nil, u: VariantUnion { b: false } };
		v
	}

	pub fn new_bool(b: bool) -> Variant
	{
		let v = Variant { t: VariantType::Bool, u: VariantUnion { b: b } };
		v
	}
	pub fn new_integer(i: i32) -> Variant
	{
		let v = Variant { t: VariantType::Integer, u: VariantUnion { i: i } };
		v
	}
	pub fn new_float(f: f64) -> Variant
	{
		let v = Variant { t: VariantType::Float, u: VariantUnion { f: f } };
		v
	}
	pub fn new_string(s: &str) -> Variant
	{
		let v = Variant { t: VariantType::String, u: VariantUnion { s: Box::into_raw(Box::new(VariantStdString::new(s))) } };
		v
	}

	pub fn to_bool(&self) -> bool
	{
		match self.t {
			VariantType::Bool => {
				let b = unsafe { self.u.b };
				return b
			},
			_ => panic!("Variant is not a bool"),
		}
	}

	pub fn to_integer(&self) -> i32
	{
		match self.t {
			VariantType::Integer => {
				let i = unsafe { self.u.i };
				return i
			},
			_ => panic!("Variant is not an integer"),
		}
	}

	pub fn to_float(&self) -> f64
	{
		match self.t {
			VariantType::Float => {
				let f = unsafe { self.u.f };
				return f
			},
			_ => panic!("Variant is not a float"),
		}
	}

	pub fn to_string(&self) -> String
	{
		match self.t {
			VariantType::Nil => return "nil".to_string(),
			VariantType::Bool => {
				let b = unsafe { self.u.b };
				return b.to_string()
			},
			VariantType::Integer => {
				let i = unsafe { self.u.i };
				return i.to_string()
			},
			VariantType::Float => {
				let f = unsafe { self.u.f };
				return f.to_string()
			},
			VariantType::String => {
				let s = unsafe { &*self.u.s };
				let slice = unsafe { std::slice::from_raw_parts(s.ptr as *const u8, s.len) };
				return String::from_utf8(slice.to_vec()).unwrap();
			},
		}
	}
}

impl VariantStdString
{
	pub fn new(s: &str) -> VariantStdString
	{
		if s.len() <= 16 {
			let mut sso = [0; 16];
			for i in 0..s.len() {
				sso[i] = s.as_bytes()[i];
			}
			let v = VariantStdString { ptr: s.as_ptr() as *const char, len: s.len(), cap_or_sso: CapacityOrSSO { sso: sso } };
			return v;
		}

		let v = VariantStdString { ptr: s.as_ptr() as *const char, len: s.len(), cap_or_sso: CapacityOrSSO { cap: s.len() } };
		v
	}
}

const SYSCALL_PRINT: i64 = 500;
const SYSCALL_THROW: i64 = 511;

// The print system call takes a pointer to an array of Variant structs in A0,
// as well as the number of elements in the array in A1.
pub fn print1(v: &Variant)
{
	unsafe {
		asm!("ecall",
			in("a0") v,
			in("a1") 1,
			in("a7") SYSCALL_PRINT,
			lateout("a0") _,
			options(nostack));
	}
}
pub fn print(v: &[Variant])
{
	unsafe {
		asm!("ecall",
			in("a0") v.as_ptr(),
			in("a1") v.len(),
			in("a7") SYSCALL_PRINT,
			lateout("a0") _,
			options(nostack));
	}
}

pub fn test_print()
{
	let v1 = Variant::new_integer(42);
	let v2 = Variant::new_float(3.14);
	let v3 = Variant::new_string("Hello, world!");
	let v = [v1, v2, v3];
	print(&v);
}
