shady.set("spatial_mode", true)
shady.set("physics_enabled", false)
shady.set("fps_mode", false)
shady.set("sky", false)
shady.modules({ ["physics"] = false, ["fps"] = false, ["lua"] = true })
-- No plugins.load: a .glb environment makes Shady load glb-loader by name.
shady.plugins.path(assert(os.getenv("SHADY_TEST_PLUGIN_DIR")))
shady.set("environment", true)
shady.set("environment_path", assert(os.getenv("SHADY_TEST_GLB")))
