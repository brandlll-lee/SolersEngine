def can_build(env, platform):
    # Reuse Godot's existing Manifold integration for robust solid booleans.
    env.module_add_dependencies("solers_modeling", ["csg", "meshoptimizer", "xatlas_unwrap"], True)
    return env.editor_build


def configure(env):
    pass
