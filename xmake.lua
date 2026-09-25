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

add_requires("socket-io-client 3.1.0", { configs = { shared = false, cxxflags = "-DSIO_TLS" } })
add_requires("imgui v1.92.9", { configs = { dx11 = true, win32 = true } })
add_requires("simpleini v4.25", { configs = { convert = "none" } })
set_runtimes("MD")

-- define targets
target("skyrim-mp")
    add_rules("commonlibsse-ng.plugin", {
        name = "skyrim-mp",
        author = "B4lex",
        description = "SKSE64 plugin template using CommonLibSSE-NG"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    add_packages("socket-io-client", "imgui", "simpleini")
    -- socket-io-client uses Winsock through standalone Asio on Windows.
    add_syslinks("ws2_32")
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
