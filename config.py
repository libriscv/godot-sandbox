def can_build(env, platform):
    return True


def configure(env):
    pass

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
