includes("lib/commonlibsse-ng")
includes("locale")

set_project("PlayerHub")
set_version("1.0.0")
set_license("GPL-3.0-or-later")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

option("tracy")
    set_default(false)
    set_showmenu(true)
    set_description("Build with Tracy profiler instrumentation")
option_end()

-- Release builds set it to the public server, which the repository does not name.
option("server_endpoint")
    set_default("http://127.0.0.1:3000")
    set_showmenu(true)
    set_description("Official server URL built into the plugin, starting with http:// or https://")
option_end()

-- Without it the settings window has no Discord link.
option("discord_invite")
    set_default("")
    set_showmenu(true)
    set_description("Discord invite URL the settings window links to, starting with https://")
option_end()

-- Rebuild a package when a dependency's version changes. Without this, a cached
-- IXWebSocket compiled against one mbedtls major version is linked against
-- another; its API selection is compile-time, so TLS setup fails at runtime.
set_policy("package.librarydeps.strict_compatibility", true)

-- The xmake-repo recipe with patches/ixwebsocket applied. A distinct name, because
-- set_base looks up project packages first and would find this one.
package("ixwebsocket-patched")
    set_base("ixwebsocket")
    -- The base's deps are inherited only while this package has none, and the
    -- base's on_load adds the TLS one here, which dropped zlib and cmake.
    add_deps("zlib", "cmake")
    add_patches("v12.0.1", path.join(os.scriptdir(), "patches", "ixwebsocket", "happy-eyeballs.patch"),
        "a4b1d6b3769289f6e775d757fc25ff7805862276d7e4e2adf22fc321a25d4de3")
package_end()

add_requires("ixwebsocket-patched v12.0.1", { configs = { use_tls = true, ssl = "mbedtls" } })
-- mbedtls 4 returns MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET from
-- mbedtls_ssl_read for every TLS 1.3 ticket, which IXWebSocket 12 treats as a
-- failed read. mbedtls 3.6 ignores tickets unless the client opts in.
add_requireconfs("ixwebsocket-patched.mbedtls", { version = "v3.6.7", override = true })
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

target("PlayerHub")
    add_rules("commonlibsse-ng.plugin", {
        name = "PlayerHub",
        author = "B4lex",
        description = "Shows other players as actors in your own Skyrim world"
    })

    add_rules("locale")
    -- Translated text literals are UTF-8.
    set_encodings("utf-8")
    add_files("src/**.cpp", "src/**.c")
    -- nanopb runtime from the lib/nanopb submodule, because the xmake package
    -- depends on protoc, which cannot run under wine. Keep its tag in step with
    -- the generator version in proto/generate.sh, which produces the code in
    -- src/network/proto.
    add_files("lib/nanopb/*.c")
    add_headerfiles("src/**.h")
    add_includedirs("src", "src/network/proto", "lib/nanopb")
    add_packages("ixwebsocket-patched", "imgui", "simpleini")
    on_config(function (target)
        -- The value becomes a C string literal.
        local endpoint = get_config("server_endpoint") or ""
        if not (endpoint:startswith("http://") or endpoint:startswith("https://")) or endpoint:find("[%s\"\\]") then
            raise("server_endpoint must start with http:// or https:// and contain no whitespace, quotes, or backslashes")
        end
        target:add("defines", format('PLAYERHUB_DEFAULT_SERVER_ENDPOINT="%s"', endpoint))
        -- Defined even when empty: xmake recompiles for a changed define, not an added one.
        local invite = get_config("discord_invite") or ""
        if invite ~= "" and (not invite:startswith("https://") or invite:find("[%s\"\\]")) then
            raise("discord_invite must start with https:// and contain no whitespace, quotes, or backslashes")
        end
        target:add("defines", format('PLAYERHUB_DISCORD_INVITE="%s"', invite))
    end)
    if has_config("tracy") then
        add_packages("tracy")
        -- The package does not export TRACY_ENABLE to consumers.
        add_defines("TRACY_ENABLE")
    end
    set_pcxxheader("src/pch.h")

after_build(function (target)

    local install_dirs = os.dirs(path.join(os.projectdir(), "install_dirs", "*"))

    local dllfile = target:targetfile()
    local pdbfile = target:symbolfile()
    local fontfiles = path.join(os.projectdir(), "assets", "fonts", "*")
    local framefiles = path.join(os.projectdir(), "assets", "frame", "*")
    -- The plugin reads the translations at startup; the template is for translators.
    local localefiles = { path.join(os.projectdir(), "locale", "*.po"), path.join(os.projectdir(), "locale", "*.pot") }
    -- The licenses must go with every copy of the DLL; .txt so Windows opens them.
    local licensefiles = {
        { "LICENSE", "LICENSE.txt" },
        { "EXCEPTIONS", "EXCEPTIONS.txt" },
        { "THIRD_PARTY_NOTICES.txt", "THIRD_PARTY_NOTICES.txt" }
    }
    for _, install_dir in ipairs(install_dirs) do
        os.cp(dllfile, install_dir)

        if pdbfile and os.isfile(pdbfile) then
            os.cp(pdbfile, install_dir)
        end

        local fonts_dir = path.join(install_dir, "PlayerHub", "fonts")
        os.mkdir(fonts_dir)
        os.cp(fontfiles, fonts_dir)

        local frame_dir = path.join(install_dir, "PlayerHub", "frame")
        os.mkdir(frame_dir)
        os.cp(framefiles, frame_dir)

        local locale_dir = path.join(install_dir, "PlayerHub", "locale")
        os.mkdir(locale_dir)
        for _, files in ipairs(localefiles) do
            os.cp(files, locale_dir)
        end

        for _, file in ipairs(licensefiles) do
            os.cp(path.join(os.projectdir(), file[1]), path.join(install_dir, "PlayerHub", file[2]))
        end

        print("Installed to %s", install_dir)
    end
end)
