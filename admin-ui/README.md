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
  lib/                 framework-free helpers and their tests; predictions.ts holds what each command does to the snapshot
  styles/              design tokens (dark and light) and base styles
```

The server pushes a complete snapshot of everything on connect and after each
change, and `ServerStateProvider` exposes it through `useServerState()`. Pages
read from it and send commands through `api`; there is no per-page polling or
fetching except for detail that is not part of the snapshot (a recording's
timeline markers, and the Sessions page, which fetches one page of sessions
again when its search, role filter, or page changes, after its own commands,
and when the snapshot's connected players, bans, or roles' holders change). A command that returns the changed object (a playback) is
adopted at once, and whichever of it and the next push is newer, by `revision`,
wins.

No command makes the panel wait for the server before it shows something:

- A command on something already on screen (hide, kick, stop, delete, pause,
  seek...) passes `useAction`'s `run` a `Prediction` from `lib/predictions.ts`.
  `useServerState()` returns the snapshot with the predictions applied, so every
  page shows the outcome from the click. A refused command takes it back and
  says why in a toast; otherwise the prediction is dropped once a snapshot shows
  the same thing.
- A command that creates something (a capture, a replay, a batch) has no
  outcome to predict, since the server names it. Its `Button` is `busy`, which
  blocks clicks at once and shows a spinner only if the answer takes longer than
  `--busy-delay`, and the object in the response is listed without waiting for
  the next push.

New commands should follow the same split, with a new prediction next to the
existing ones.

While a replay plays the server does not push positions. The panel advances the
last reported one with its own clock (`lib/playbackClock.ts`), which is why the
timeline moves smoothly and why pushes stay small.

## Adding a page

1. Create `src/features/<name>/<Name>Page.tsx`, using `PageHeader`, `Panel`, and
   `useServerState()`.
2. Add one entry to `pages` in `src/pages.tsx` (path, sidebar label, icon).
3. If it needs data the snapshot does not carry, add the field to `State` in
   `../server/internal/admin/state.go`, where it is also filled, and to
   `AdminState` in `src/api/types.ts`. Commands are new routes in
   `../server/internal/admin/api.go` and a matching function in
   `src/api/client.ts`.

Add an icon to the `glyphs` map in `components/Icon.tsx` if the page needs one.
