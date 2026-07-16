def can_build(env, platform):
    env.module_add_dependencies("solers_ai", ["freetype", "svg", "zip"], True)
    return True


def configure(env):
    pass
