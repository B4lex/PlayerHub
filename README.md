# PlayerHub

Non-intrusive multiplayer for Skyrim. Each player keeps their own world and sees
other players as remote actors: movement, animations, combat, appearance, and
equipment are shared, while loot, quests, and enemies stay local. Players can
trade items and chat.

- `src/` — SKSE plugin (C++23, built with [XMake](https://xmake.io): `xmake`)
- `server/` — Go server ([server/README.md](server/README.md))
- `proto/` — wire schema
- `specs/` — how each system works ([specs/README.md](specs/README.md))

## Licenses

The plugin and server are under the GNU General Public License 3.0
([LICENSE](LICENSE)), with the exceptions in [EXCEPTIONS](EXCEPTIONS).

The settings window is set in [Sovngarde](https://mjorka.itch.io/sovngarde) by
mjorka, shipped unmodified in [assets/fonts](assets/fonts) under the SIL Open
Font License 1.1
([Sovngarde OFL License.txt](assets/fonts/Sovngarde%20OFL%20License.txt)). The
font files are not covered by the plugin's license, and the license text goes
with them wherever they are distributed.
