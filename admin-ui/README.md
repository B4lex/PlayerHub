# PlayerHub admin panel

React and TypeScript front end for the game server's admin API. The server
hosts the built app at `/admin/` on its admin address (`admin_listen`,
`http://127.0.0.1:3001/admin/` by default); see `../README.md` for the API it
talks to.

## Build and run

```sh
npm install
npm run build     # type-checks, then writes dist/, which the server serves at /admin/
```

The server reads `dist/` from disk on every request, so building or rebuilding
needs no restart.

For development, run the game server, then:

```sh
npm run dev       # http://localhost:5173/admin/, with hot reload
```

Vite forwards `/api`, including the `/api/events` feed, to `http://127.0.0.1:3001`. Point it at a
server on another admin address with `ADMIN_SERVER_URL=http://127.0.0.1:3101 npm run dev`.

Other scripts: `npm run typecheck`, `npm test` (Vitest, for the pure logic in
`src/lib/`).

## How it fits together

```text
src/
  pages.tsx            the page list: sidebar and routes are both built from it
  api/                 types.ts (wire types), client.ts (REST), feed.ts (live state and chat)
  state/               ServerState (one live snapshot for every page, and chat lines), useAction (commands)
  components/          shared pieces: Button, Panel, Badge, StatTile, Toasts, Sidebar...
  features/<page>/     one folder per page, with its own components and CSS modules
  lib/                 framework-free helpers and their tests
  styles/              design tokens (dark and light) and base styles
```

The server pushes a complete snapshot of everything on connect and after each
change, and `ServerStateProvider` exposes it through `useServerState()`. Pages
read from it and send commands through `api`; there is no per-page polling or
fetching except for detail that is not part of the snapshot (a recording's
timeline markers, and the Sessions page, which fetches one page of sessions
again when its search or page changes, after its own commands, and when the
snapshot's connected players or bans change). A command that returns the changed object (a playback) is
adopted at once, and whichever of it and the next push is newer, by `revision`,
wins.

While a replay plays the server does not push positions. The panel advances the
last reported one with its own clock (`lib/playbackClock.ts`), which is why the
timeline moves smoothly and why pushes stay small.

## Adding a page

1. Create `src/features/<name>/<Name>Page.tsx`, using `PageHeader`, `Panel`, and
   `useServerState()`.
2. Add one entry to `pages` in `src/pages.tsx` (path, sidebar label, icon).
3. If it needs data the snapshot does not carry, add the field to
   `AdminState` in `../src/server/admin/schemas.py` and `src/api/types.ts`, and
   fill it in `../src/server/admin/state.py`. Commands are new routes in
   `../src/server/admin/api.py` and a matching function in `src/api/client.ts`.

Add an icon to the `glyphs` map in `components/Icon.tsx` if the page needs one.
