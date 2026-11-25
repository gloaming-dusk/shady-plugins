-- Give the wanderer a model, let it walk, hot-reload it mid-walk, take a
-- screenshot, then unload it.
shady.publish("glb-wanderer.model", assert(os.getenv("SHADY_TEST_GLB")))
shady.publish("glb-wanderer.height", "0.5")
local after = shady.automation.after

after(1500, function()
    assert(shady.plugins.reload("glb-wanderer"), "reload refused")
    after(1500, function()
        shady.screenshot(assert(os.getenv("SHADY_TEST_SHOT")))
        after(800, function()
            assert(shady.plugins.unload("glb-wanderer"), "unload refused")
            shady.log("glb-wanderer-test: PASS")
            after(200, function() shady.quit() end)
        end)
    end)
end)
