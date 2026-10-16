# PlayerHub

Non-intrusive multiplayer for Skyrim. Each player keeps their own world and sees
other players as remote actors: movement, animations, combat, appearance, and
equipment are shared, while loot, quests, and enemies stay local. Players can
trade items and chat.

- `src/` — SKSE plugin (C++23, built with [XMake](https://xmake.io): `xmake`)
- `server/` — Go server ([server/README.md](server/README.md))
- `proto/` — wire schema
- `specs/` — how each system works ([specs/README.md](specs/README.md))

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

The plugin and server are under the GNU General Public License 3.0
([LICENSE](LICENSE)), with the exceptions in [EXCEPTIONS](EXCEPTIONS).

The settings window is set in [Sovngarde](https://mjorka.itch.io/sovngarde) by
mjorka, shipped unmodified in [assets/fonts](assets/fonts) under the SIL Open
Font License 1.1
([Sovngarde OFL License.txt](assets/fonts/Sovngarde%20OFL%20License.txt)). The
font files are not covered by the plugin's license, and the license text goes
with them wherever they are distributed.
