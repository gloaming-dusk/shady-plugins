local name = assert(os.getenv("SHADY_TEST_PLUGIN"))
local function state()
    for _, p in ipairs(shady.plugins.list()) do
        if p.spec == name then return p.state end
    end
end
shady.automation.after(800, function()
    if state() ~= "active" then
        shady.log("smoke-test: state is " .. tostring(state()))
        return shady.quit()
    end
    -- A plugin with state on the old ABI may refuse, but must keep running.
    local reloaded = shady.plugins.reload(name)
    shady.log("smoke-test: reload " .. (reloaded and "done" or "refused"))
    shady.automation.after(400, function()
        if state() == "active" then
            shady.log("smoke-test: PASS")
        else
            shady.log("smoke-test: state after reload is " .. tostring(state()))
        end
        shady.quit()
    end)
end)
