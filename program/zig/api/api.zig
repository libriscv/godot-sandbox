//
// Godot Sandbox Zig API
//
pub const V = union { b: bool, i: i64, f: f64, bytes: [16]u8 };

pub const Variant = struct {
    type: i64,
    v: V,

    pub fn init_int(self: *Variant, int: i64) void {
        self.type = 2; // INT
        self.v.i = int;
    }
};
