def can_build(env, platform):
    if env["platform"] == "web" and env.get("disable_exceptions", True):
        print("Sandbox module cannot be built for web with exceptions enabled.")
        return False
    # All platforms minus windows without mingw
    return (env["platform"] == "windows" and env.get("use_mingw", False)) or env["platform"] != "windows"


def configure(env):
    True

def get_doc_classes():
    return [
        "Sandbox",
        "ELFScript",
        "ELFScriptLanguage",
        "CPPScript",
        "CPPScriptLanguage",
        "RustScript",
        "RustScriptLanguage",
        "ZigScript",
        "ZigScriptLanguage",
    ]

def get_doc_path():
    return "doc_classes"
