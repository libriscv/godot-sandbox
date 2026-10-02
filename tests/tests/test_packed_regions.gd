extends GutTest

# What a guest can do with ECALL_PACKED_ACQUIRE and ECALL_PACKED_RELEASE that compiled
# SafeGDScript never does: each case ends in a guest exception or a defined store, and the
# host's arrays and heap stay whole. The guest side is tests/test_packed_regions.cpp.

var Sandbox_TestsTests = load("res://tests/tests.elf")


func _holder() -> Array:
	return [PackedFloat32Array([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0])]


func test_a_replaced_array_gets_no_store_when_the_region_unwinds():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	var holder := _holder()
	var before: PackedFloat32Array = holder[0].duplicate()
	var exceptions: int = s.get_exceptions()
	assert_null(s.vmcall("packed_region_replaced_then_fault", holder), "the guest faulted")
	assert_eq(s.get_exceptions(), exceptions + 1, "a guest exception, not a host crash")
	assert_eq(holder[0], before, "the array the slot no longer holds is not written")
	assert_engine_error("replaced before the region ended")
	s.free()


func test_a_freed_array_gets_no_store_when_the_region_unwinds():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	var exceptions: int = s.get_exceptions()
	assert_null(s.vmcall("packed_region_freed_then_fault"), "the guest faulted")
	assert_eq(s.get_exceptions(), exceptions + 1, "a guest exception, not a host crash")
	assert_engine_error("replaced before the region ended")
	s.free()


func test_a_region_left_without_a_release_ends_with_its_call():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	var holder := _holder()
	assert_eq(s.vmcall("packed_region_left_unreleased", holder), "returned without a release")
	var expected := PackedFloat32Array()
	expected.resize(10)
	expected.fill(42.0)
	assert_eq(holder[0], expected, "the copy is stored when the call returns, while the slot still holds its array")
	s.free()


func test_a_release_the_guest_cannot_supply_leaves_the_array_alone():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	var holder := _holder()
	var before: PackedFloat32Array = holder[0].duplicate()
	var exceptions: int = s.get_exceptions()
	assert_null(s.vmcall("packed_release_past_guest_memory", holder), "the release was refused")
	assert_eq(s.get_exceptions(), exceptions + 1, "a guest exception")
	assert_eq(holder[0], before, "neither resized nor written")
	s.free()


func test_a_huge_expected_access_count_does_not_overflow():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	assert_eq(s.vmcall("packed_acquire_with_huge_expected", _holder()), 0,
			"ten elements are worth copying for INT64_MAX accesses")
	s.free()


func test_a_release_frees_only_the_copy_it_acquired():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	var exceptions: int = s.get_exceptions()
	assert_eq(s.vmcall("packed_release_frees_only_its_copy", _holder()), "freed once",
			"a block the guest names in a release with no acquire is still the guest's to free")
	assert_eq(s.get_exceptions(), exceptions, "no double free")
	s.free()
