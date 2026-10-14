-- Compiles the text catalogs generate.lua makes from the gettext files here.
rule("locale")
    on_load(function (target)
        import("generate")
        local outdir = path.join(target:autogendir(), "rules", "locale")
        target:add("includedirs", outdir)
        for _, file in ipairs(generate.sources()) do
            -- The files do not exist until the prepare stage writes them.
            target:add("files", path.join(outdir, file), { always_added = true })
        end
    end)
    on_prepare(function (target, opt)
        import("generate")
        import("utils.progress")
        local written, summary = generate.run(path.join(target:autogendir(), "rules", "locale"))
        if #written > 0 then
            progress.show(opt.progress, "${color.build.object}generating.locale %d files", #written)
        end
        for _, line in ipairs(summary) do
            if line.translated < line.total then
                cprint("${color.warning}locale/%s.po: %d of %d texts translated; the rest are English",
                    line.code, line.translated, line.total)
            end
        end
    end)
