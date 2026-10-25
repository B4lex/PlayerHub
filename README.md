# PlayerHub

Non-intrusive multiplayer for Skyrim. Each player keeps their own world and sees
other players as remote actors: movement, animations, combat, appearance, and
equipment are shared, while loot, quests, and enemies stay local. Players can
trade items and chat.

- `src/` — SKSE plugin (C++23, built with [XMake](https://xmake.io): `xmake`)
- `server/` — Go server ([server/README.md](server/README.md))
- `proto/` — wire schema
- `specs/` — how each system works ([specs/README.md](specs/README.md))

## Docker

[Dockerfile](Dockerfile) builds the server and the admin panel into one image.
[compose.yaml](compose.yaml) runs it with host networking, so `listen`,
`admin_listen`, and `trusted_proxies` mean what they do without Docker. Copy
`server/config.example.toml` to `server/config.toml`, add
`admin_ui_directory = "/app/admin-ui"`, then:

```sh
mkdir -p data
docker compose up -d --build
```

The session database and recordings are kept in `data/` next to
`compose.yaml` (`/data` in the container). The server runs as user and group
1000, so that directory must be writable by them; set `PLAYERHUB_UID` and
`PLAYERHUB_GID` to run it as another host user, such as the one that owns
`data/`. The container restarts unless stopped.

The [server image workflow](.github/workflows/server-image.yml) publishes the
image for `linux/amd64` and `linux/arm64` as
`ghcr.io/b4lex/playerhub-server`: `main` and `sha-<commit>` from each push
to `main` that changes the server, the admin panel, or the image, and
`<version>`, `<major>.<minor>`, and `latest` from each release tag such as
`1.0.1`. Pull requests build it without publishing. To run a published image
instead of building one, replace `build` in `compose.yaml` with
`image: ghcr.io/b4lex/playerhub-server:latest`.

## Translations

The plugin's text is English, and it reads translations at startup from
gettext PO files in `Data/SKSE/Plugins/PlayerHub/locale/`. To add a language,
make `<code>.po` (such as `uk.po`) from `playerhub.pot` in that folder with a
PO editor or `msginit`, set its `Language` and `Plural-Forms` headers, and
translate it. If the game has that language, also set `X-Skyrim-Language` to
its `sLanguage` value, such as `FRENCH`. Restart the game, and the language
appears in the settings by the name given in its `LanguageName` entry. The
log names any entry the plugin could not use. The translations shipped with
the plugin are in [locale](locale), and
[specs/ui-overlay.md](specs/ui-overlay.md#translation-files) has the details.

## Licenses

Copyright (C) 2026 B4lex

The plugin and server are licensed under [GPL-3.0-or-later](LICENSE) WITH
[Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS):
you can redistribute and modify them under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version, with those exceptions. They are
distributed in the hope that they will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

[EXCEPTIONS](EXCEPTIONS) also holds additional terms under section 7 of the
license: a page that offers this program or a work based on it for download
must credit it as based on PlayerHub by B4lex with a link to this repository,
and a modified version must use another name and not the PlayerHub logo.

The plugin includes third-party libraries, among them
[CommonLibSSE NG](https://github.com/alandtse/CommonLibSSE-NG) under the same
license and exceptions. Each library and its license are listed in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt), which goes with every copy
of the plugin together with [LICENSE](LICENSE) and [EXCEPTIONS](EXCEPTIONS).

The settings window is set in [Sovngarde](https://mjorka.itch.io/sovngarde) by
mjorka, shipped unmodified in [assets/fonts](assets/fonts) under the SIL Open
Font License 1.1
([Sovngarde OFL License.txt](assets/fonts/Sovngarde%20OFL%20License.txt)). The
font files are not covered by the plugin's license, and the license text goes
with them wherever they are distributed.

The frame around the settings window and the chat,
[assets/frame/menu-frame.shape](assets/frame/menu-frame.shape), is exported from
the message box art in Skyrim's `Interface/messagebox.swf`. That art belongs to
Bethesda Softworks and is not covered by the plugin's license; it is included
only so the overlay matches the game's own menus, for use with a copy of
Skyrim. It is kept in its own file rather than built into the plugin for this
reason, and the plugin works without it.

The Discord symbol beside the community invitation,
[assets/logo/discord.svg](assets/logo/discord.svg) and its glyph in
`assets/fonts/PlayerHub.ttf`, is a trademark of Discord Inc., used unmodified
in Discord's color under its [brand guidelines](https://discord.com/branding)
to link to the mod's Discord server. It is not covered by the plugin's license,
and PlayerHub is not affiliated with or endorsed by Discord.
