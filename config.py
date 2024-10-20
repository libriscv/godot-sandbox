def can_build(env, platform):
    if platform == "windows" and env.mingw == False:
        return False
    return True


def configure(env):
    pass
