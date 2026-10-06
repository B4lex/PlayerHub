includes("lib/commonlibsse-ng")

set_project("skyrim-mp")
set_version("0.1.3")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

option("tracy")
    set_default(false)
    set_showmenu(true)
    set_description("Build with Tracy profiler instrumentation")
option_end()

-- Rebuild a package when a dependency's version changes. Without this, a cached
-- IXWebSocket compiled against one mbedtls major version is linked against
-- another; its API selection is compile-time, so TLS setup fails at runtime.
set_policy("package.librarydeps.strict_compatibility", true)

add_requires("ixwebsocket v12.0.1", { configs = { use_tls = true, ssl = "mbedtls" } })
-- mbedtls 4 returns MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET from
-- mbedtls_ssl_read for every TLS 1.3 ticket, which IXWebSocket 12 treats as a
-- failed read. mbedtls 3.6 ignores tickets unless the client opts in.
add_requireconfs("ixwebsocket.mbedtls", { version = "v3.6.7", override = true })
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

target("skyrim-mp")
    add_rules("commonlibsse-ng.plugin", {
        name = "skyrim-mp",
        author = "B4lex",
        description = "SKSE64 plugin template using CommonLibSSE-NG"
    })

    add_files("src/**.cpp", "src/**.c")
    -- nanopb runtime from the lib/nanopb submodule, because the xmake package
    -- depends on protoc, which cannot run under wine. Keep its tag in step with
    -- the generator version in proto/generate.sh, which produces the code in
    -- src/network/proto.
    add_files("lib/nanopb/*.c")
    add_headerfiles("src/**.h")
    add_includedirs("src", "src/network/proto", "lib/nanopb")
    add_packages("ixwebsocket", "imgui", "simpleini")
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
    local fontfiles = path.join(os.projectdir(), "assets", "fonts", "*")
    local framefiles = path.join(os.projectdir(), "assets", "frame", "*")
    for _, deploy_dir in ipairs(deploy_dirs) do
        os.cp(dllfile, deploy_dir)

        if pdbfile and os.isfile(pdbfile) then
            os.cp(pdbfile, deploy_dir)
        end

        local fonts_dir = path.join(deploy_dir, "skyrim-mp", "fonts")
        os.mkdir(fonts_dir)
        os.cp(fontfiles, fonts_dir)

        local frame_dir = path.join(deploy_dir, "skyrim-mp", "frame")
        os.mkdir(frame_dir)
        os.cp(framefiles, frame_dir)

        print("Deployed to %s", deploy_dir)
    end
end)
