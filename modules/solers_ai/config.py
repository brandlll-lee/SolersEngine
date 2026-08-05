def can_build(env, platform):
    env.module_add_dependencies("solers_ai", ["freetype", "svg", "zip"], True)
    return env.editor_build


def configure(env):
    pass
