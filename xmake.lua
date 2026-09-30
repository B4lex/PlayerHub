-- include subprojects
includes("lib/commonlibsse-ng")

-- set project constants
set_project("skyrim-mp")
set_version("0.1.3")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

option("tracy")
    set_default(false)
    set_showmenu(true)
    set_description("Build with Tracy profiler instrumentation")
option_end()

add_requires("ixwebsocket v12.0.1", { configs = { use_tls = true, ssl = "mbedtls" } })
-- The generated code in src/network/proto comes from proto/generate.sh.
add_requires("nanopb 0.4.9")
add_requires("imgui v1.92.9", { configs = { dx11 = true, win32 = true } })
add_requires("simpleini v4.25", { configs = { convert = "none" } })
if has_config("tracy") then
    -- Tracy 0.14 defaults its CMake TRACY_ENABLE option to OFF, and the xmake-repo
    -- recipe does not set it, which leaves TracyClient empty. It also removed the
    -- delayed_init option; the recipe would still export that define to consumers.
    add_requires("tracy v0.14.1", { configs = {
        cxxflags = "-DTRACY_ENABLE",
        on_demand = true,
        callstack = false,
        enforce_callstack = false,
        crash_handler = false
    } })
end
set_runtimes("MD")

-- define targets
target("skyrim-mp")
    add_rules("commonlibsse-ng.plugin", {
        name = "skyrim-mp",
        author = "B4lex",
        description = "SKSE64 plugin template using CommonLibSSE-NG"
    })

    -- add src files
    add_files("src/**.cpp", "src/**.c")
    add_headerfiles("src/**.h")
    add_includedirs("src", "src/network/proto")
    add_packages("ixwebsocket", "nanopb", "imgui", "simpleini")
    if has_config("tracy") then
        add_packages("tracy")
        -- The package does not export TRACY_ENABLE to consumers.
        add_defines("TRACY_ENABLE")
    end
    set_pcxxheader("src/pch.h")

after_build(function (target)

    local deploy_root = path.join(os.projectdir(), "deploy_dirs")
    local deploy_dirs = os.dirs(path.join(deploy_root, "*"))

    if #deploy_dirs == 0 then
        raise("No deployment directories found in %s", deploy_root)
    end

    local dllfile = target:targetfile()
    local pdbfile = target:symbolfile()
    for _, deploy_dir in ipairs(deploy_dirs) do
        os.cp(dllfile, deploy_dir)

        if pdbfile and os.isfile(pdbfile) then
            os.cp(pdbfile, deploy_dir)
        end

        print("Deployed to %s", deploy_dir)
    end
end)
