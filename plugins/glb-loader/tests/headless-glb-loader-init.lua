-- Reload the loader, point the environment at a broken file and back, then
-- unload. Each step waits for a frame, where the host syncs the config.
local glb = assert(os.getenv("SHADY_TEST_GLB"))
local broken = assert(os.getenv("SHADY_TEST_BROKEN_GLB"))
local after = shady.automation.after

after(400, function()
    assert(shady.plugins.reload("glb-loader"), "reload refused")
    after(250, function()
        shady.config("environment_path", broken)
        after(250, function()
            shady.config("environment_path", glb)
            after(250, function()
                assert(shady.plugins.unload("glb-loader"), "unload refused")
                shady.log("glb-loader-test: PASS")
                after(200, function() shady.quit() end)
            end)
        end)
    end)
end)
