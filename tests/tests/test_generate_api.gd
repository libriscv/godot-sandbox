extends GutTest

func test_generate_api_argument_names():
    var s : Sandbox = Sandbox.new()
    var api: String = s.generate_api("cpp", "", true)
    # Classes that inherit from a skipped class (eg. editor-only classes) are
    # skipped as well, which the generator warns about.
    assert_engine_error("Skipped classes left in class inheritance")
    assert_gt(api.length(), 0)
