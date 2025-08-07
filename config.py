def can_build(env, platform):
    return env.get("is_msvc", False) != True


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
