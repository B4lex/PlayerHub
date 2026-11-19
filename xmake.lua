includes("lib/commonlibsse-ng")
includes("locale")

set_project("PlayerHub")
set_version("3.0.0")
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

-- Tracy sends lock and memory events through one global mutex, which serializes
-- the threads that emit them and distorts the timings of everything else.
option("tracy_locks")
    set_default(false)
    set_showmenu(true)
    set_description("With tracy: record the plugin's instrumented locks")
option_end()

option("tracy_memory")
    set_default(false)
    set_showmenu(true)
    set_description("With tracy: record every plugin allocation")
option_end()

option("time_breakdown")
    set_default(false)
    set_showmenu(true)
    set_description("Show where plugin time goes, with hook timings, under the status panel")
option_end()

option("server_address")
    set_default("127.0.0.1")
    set_showmenu(true)
    set_description("Official server built into the plugin: host or host:port (14201 if omitted)")
option_end()

option("server_tls")
    set_default(true)
    set_showmenu(true)
    set_description("Connect to the official server over TLS")
option_end()

-- Without it the settings window has no Discord link.
option("discord_invite")
    set_default("")
    set_showmenu(true)
    set_description("Discord invite URL the settings window links to, starting with https://")
option_end()

-- Without it the settings window has no Ko-fi link.
option("kofi_url")
    set_default("")
    set_showmenu(true)
    set_description("Ko-fi page URL the settings window links to, starting with https://")
option_end()

-- Rebuild a package when a dependency's version changes. Without this, a cached
-- IXWebSocket compiled against one mbedtls major version is linked against
-- another; its API selection is compile-time, so TLS setup fails at runtime.
set_policy("package.librarydeps.strict_compatibility", true)

-- xmake probes each raw flag by compiling a stub and caches a failure for the
-- configuration. Under wine the probe can fail spuriously, which silently dropped
-- CommonLib's "/Zc:templateScope" from debug builds although MSVC supports it.
-- MSVC itself only warns (D9002) on an option it does not know.
set_policy("check.auto_ignore_flags", false)

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

package("rnnoise")
    set_homepage("https://gitlab.xiph.org/xiph/rnnoise")
    set_description("Recurrent neural network for audio noise reduction")
    set_license("BSD-3-Clause")
    set_urls("https://github.com/xiph/rnnoise/releases/download/v$(version)/rnnoise-$(version).tar.gz")
    add_versions("0.2", "90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37")
    on_install(function (package)
        -- The release's C fallback of its vector code includes os_support.h, which the archive
        -- lacks, so the build takes the SSE2 path every x64 CPU has. MSVC does not define
        -- __SSE2__; src/x86/x86_arch_macros.h derives it from the OPUS_X86_MAY_HAVE_* defines.
        io.writefile("xmake.lua", [[
            add_rules("mode.debug", "mode.release")
            target("rnnoise")
                set_kind("static")
                set_languages("c11")
                add_defines("OPUS_X86_MAY_HAVE_SSE", "OPUS_X86_MAY_HAVE_SSE2")
                add_files("src/denoise.c", "src/rnn.c", "src/pitch.c", "src/kiss_fft.c", "src/celt_lpc.c",
                    "src/nnet.c", "src/nnet_default.c", "src/parse_lpcnet_weights.c", "src/rnnoise_data.c",
                    "src/rnnoise_tables.c")
                add_includedirs("include", "src")
                add_headerfiles("include/rnnoise.h")
        ]])
        import("package.tools.xmake").install(package)
    end)
package_end()

package("libopus-dnn")
    set_homepage("https://opus-codec.org")
    set_description("Modern audio compression for the internet, with its neural decoder features")
    set_license("BSD-3-Clause")
    set_urls("https://downloads.xiph.org/releases/opus/opus-$(version).tar.gz")
    add_versions("1.5.2", "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1")
    add_deps("cmake")
    on_install(function (package)
        -- xmake chooses the MSVC runtime; Opus's CMakeLists would force its own.
        io.replace("CMakeLists.txt", [[set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")]], "", { plain = true })
        io.replace("CMakeLists.txt", [[set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")]], "", { plain = true })
        import("package.tools.cmake").install(package, {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=OFF",
            "-DOPUS_PACKAGE_VERSION=" .. package:version():shortstr(),
            "-DOPUS_OSCE=ON",
            "-DOPUS_BUILD_PROGRAMS=OFF",
            "-DOPUS_BUILD_TESTING=OFF"
        })
    end)
package_end()

add_requires("ixwebsocket-patched v12.0.1", { configs = { use_tls = true, ssl = "mbedtls" } })
add_requires("libopus-dnn 1.5.2")
-- Device input and output only; src/audio/MiniAudio.c holds the implementation.
add_requires("miniaudio 0.11.25", { configs = {
    headeronly = true,
    enabled_backends = { "wasapi", "dsound", "winmm" },
    decoding = false,
    encoding = false,
    engine = false,
    node_graph = false,
    resource_manager = false,
    generation = false,
    flac = false,
    mp3 = false,
    wav = false
} })
add_requires("rnnoise 0.2")
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
    set_encodings("utf-8")
    add_files("src/**.cpp", "src/**.c")
    -- nanopb runtime from the lib/nanopb submodule, because the xmake package
    -- depends on protoc, which cannot run under wine. Keep its tag in step with
    -- the generator version in proto/generate.sh, which produces the code in
    -- src/network/proto.
    add_files("lib/nanopb/*.c")
    add_headerfiles("src/**.h")
    add_includedirs("src", "src/network/proto", "lib/nanopb")
    add_packages("ixwebsocket-patched", "imgui", "simpleini", "libopus-dnn", "miniaudio", "rnnoise")
    add_syslinks("ws2_32")
    on_config(function (target)
        local address = get_config("server_address") or ""
        local host, port = address:match("^([%w%.%-]+)(.*)$")
        local number = port and port:match("^:(%d+)$")
        if not host or #host > 253 or (port ~= "" and not (number and #number <= 5 and tonumber(number) >= 1
                and tonumber(number) <= 65535)) then
            raise("server_address must be a host or host:port, such as example.org or example.org:443")
        end
        target:add("defines", format('PLAYERHUB_DEFAULT_SERVER_ADDRESS="%s"', address))
        target:add("defines", "PLAYERHUB_DEFAULT_SERVER_TLS=" .. (has_config("server_tls") and "true" or "false"))
        for _, entry in ipairs({{"discord_invite", "PLAYERHUB_DISCORD_INVITE"}, {"kofi_url", "PLAYERHUB_KOFI_URL"}}) do
            local option, define = entry[1], entry[2]
            local url = get_config(option) or ""
            if url ~= "" and (not url:startswith("https://") or url:find("[%s\"\\]")) then
                raise("%s must start with https:// and contain no whitespace, quotes, or backslashes", option)
            end
            target:add("defines", format('%s="%s"', define, url))
        end
    end)
    if has_config("time_breakdown") then
        add_defines("PLAYERHUB_TIME_BREAKDOWN")
    end
    if has_config("tracy") then
        add_packages("tracy")
        -- The package does not export TRACY_ENABLE to consumers.
        add_defines("TRACY_ENABLE")
        if has_config("tracy_locks") then
            add_defines("PLAYERHUB_TRACY_LOCKS")
        end
        if has_config("tracy_memory") then
            add_defines("PLAYERHUB_TRACY_MEMORY")
        end
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
