extends GutTest

func test_generate_api_argument_names():
    var s : Sandbox = Sandbox.new()
    var api: String = s.generate_api("cpp", "", true)
    assert_gt(api.length(), 0)