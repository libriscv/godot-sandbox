use sysalloc::*;
use api::*;

pub fn main()
{
    print!("example {:.1} test {:x} words {}",
        1, 2, 3);

    let _x = Box::new(6667);

    let mut v = vec![0, 1, 2, 3, 4, 5];

    print!("Hello, Rust world {}!", v[2]);

    for x in 0..256 {
        v.push(x);
    }

    print!("Hello, Rust world {}!", v[3]);

    print!("Big number: {} Floaty: {}", 1234567890, 1.23456789);

    let mut vec = Vec::new();
    vec.push(1);
    vec.push(2);

    print!("Done!");
}

#[no_mangle]
pub fn public_function(v: &Variant) -> Variant
{
	gprint(v);
	gprint(&Variant::new_string("Hello from Rust!"));
	gprint(&Variant::new_string("Hello from Rust!"));

	let v = Variant::new_float(3.14);
	v
}
