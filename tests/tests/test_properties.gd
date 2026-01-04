extends GutTest

var Sandbox_TestsTests = load("res://tests/tests.elf")

func validate_property(prop_list, name):
	for p in prop_list:
		if p.name == name:
			return true
	return false

func test_script_properties():
	var n = Node.new()
	n.name = "Node"
	n.set_script(Sandbox_TestsTests)

	# Verify properties exist
	var prop_list = n.get_property_list()
	assert_true(validate_property(prop_list, "player_speed"), "Property player_speed found")
	assert_true(validate_property(prop_list, "player_jump_vel"), "Property player_jump_vel found")
	assert_true(validate_property(prop_list, "player_name"), "Property player_name found")

	# Test property access
	assert_eq(n.get("player_speed"), 150.0, "Property player_speed has value 150")
	assert_eq(n.get("player_jump_vel"), -300.0, "Property player_jump_vel has value -300")
	assert_eq(n.get("player_name"), "Slide Knight", "Property player_name has value Slide Knight")

	# Test property set
	n.set("player_speed", 200.0)
	n.set("player_jump_vel", -400.0)
	n.set("player_name", "Jump Knight")

	assert_eq(n.get("player_speed"), 200.0, "Property player_speed has value 200")
	assert_eq(n.get("player_jump_vel"), -400.0, "Property player_jump_vel has value -400")
	assert_eq(n.get("player_name"), "Jump Knight", "Property player_name has value Jump Knight")

	n.queue_free()

func test_elfscript_properties():
	var n = Sandbox.new()
	n.set_program(Sandbox_TestsTests)

	# Verify properties exist
	var prop_list = n.get_property_list()
	assert_true(validate_property(prop_list, "player_speed"), "Property player_speed found")
	assert_true(validate_property(prop_list, "player_jump_vel"), "Property player_jump_vel found")
	assert_true(validate_property(prop_list, "player_name"), "Property player_name found")

	# Test property access
	assert_eq(n.get("player_speed"), 150.0, "Property player_speed has value 150")
	assert_eq(n.get("player_jump_vel"), -300.0, "Property player_jump_vel has value -300")
	assert_eq(n.get("player_name"), "Slide Knight", "Property player_name has value Slide Knight")

	# Test property set
	n.set("player_speed", 200.0)
	n.set("player_jump_vel", -400.0)
	n.set("player_name", "Jump Knight")

	assert_eq(n.get("player_speed"), 200.0, "Property player_speed has value 200")
	assert_eq(n.get("player_jump_vel"), -400.0, "Property player_jump_vel has value -400")
	assert_eq(n.get("player_name"), "Jump Knight", "Property player_name has value Jump Knight")

	n.queue_free()

func test_meshinstance3d_mesh_property():
	var mi = MeshInstance3D.new()
	mi.name = "MeshInstance3D"
	
	# Test that mesh property exists
	var prop_list = mi.get_property_list()
	assert_true(validate_property(prop_list, "mesh"), "Property mesh found")
	
	# Test initial state - mesh should be null
	assert_eq(mi.get("mesh"), null, "Initial mesh should be null")
	assert_eq(mi.mesh, null, "Initial mesh property should be null")
	assert_eq(mi.get_mesh(), null, "Initial get_mesh() should return null")
	
	# Create a test mesh (BoxMesh)
	var box_mesh = BoxMesh.new()
	
	# Test setting mesh via set_mesh method
	mi.set_mesh(box_mesh)
	assert_same(mi.get("mesh"), box_mesh, "get('mesh') should return the set mesh")
	assert_same(mi.mesh, box_mesh, "mesh property should return the set mesh")
	assert_same(mi.get_mesh(), box_mesh, "get_mesh() should return the set mesh")
	
	# Test setting mesh via property assignment
	var sphere_mesh = SphereMesh.new()
	mi.mesh = sphere_mesh
	assert_same(mi.get("mesh"), sphere_mesh, "get('mesh') should return the new mesh after property assignment")
	assert_same(mi.mesh, sphere_mesh, "mesh property should return the new mesh")
	assert_same(mi.get_mesh(), sphere_mesh, "get_mesh() should return the new mesh")
	
	# Test setting mesh via set() method
	var cylinder_mesh = CylinderMesh.new()
	mi.set("mesh", cylinder_mesh)
	assert_same(mi.get("mesh"), cylinder_mesh, "get('mesh') should return the mesh set via set()")
	assert_same(mi.mesh, cylinder_mesh, "mesh property should return the mesh set via set()")
	assert_same(mi.get_mesh(), cylinder_mesh, "get_mesh() should return the mesh set via set()")
	
	# Test setting to null
	mi.mesh = null
	assert_eq(mi.get("mesh"), null, "get('mesh') should return null after setting to null")
	assert_eq(mi.mesh, null, "mesh property should return null after setting to null")
	assert_eq(mi.get_mesh(), null, "get_mesh() should return null after setting to null")
	
	mi.queue_free()
