# Player Chat

## Status

Implemented, pending a Windows build and in-game check. The server relay is
covered by unit tests. The input dispatch hook site and the input context order
were checked against the 1.7.104 executable and address library; SE was not
checked. See `specs/010-ui-overlay.md` for the window and input layer the chat
is built on.

## Goal

Let players talk to each other in plain text. Chat is global: every connected
player receives every message. It is presentation only and never changes
anyone's world.

## Wire

| Event | Direction | Payload |
| --- | --- | --- |
| `chat:send` | client → server | `{"text": string}`, acknowledged with `{"ok": true}` or `{"ok": false, "error": ...}` |
| `chat:message` | server → every client | `{"playerId": string, "name": string, "text": string}` |

- The server trims surrounding spaces and rejects empty text, control
  characters (below U+0020, and U+007F), unencodable strings such as lone
  surrogates, and text over 512 UTF-8 bytes (`"invalid chat message"`).
- `ChatThrottle` allows 5 messages per player in any 5 seconds
  (`"rate limited"`). Rejected messages do not count, and a disconnect forgets
  the player's history.
- `name` is the sender's character name from its latest `player:appearance`,
  or empty before one arrives. The client shows an empty name as `Unknown`.
- The sender receives its own message too, so every player sees the same
  order, and a message the sender sees was delivered.
- Chat does not require being in the world, keeps no history for late joiners,
  and is not captured in recordings.

## Client

```text
src/protocol/ChatMessage.h      # message type, byte limit, shared text validation
src/network/SocketIOClient      # chat:send with ack, chat:message queue
src/sync/chat/ChatSync          # send, rejections as notices, lines to display
src/ui/chat/ChatWindow          # window: history, fading recent lines, input line
```

- `Network::PublishChatMessage` emits `chat:send`; a rejecting ack runs its
  callback on the transport thread. `chat:message` payloads are validated like
  the server's and queued, at most 128, dropping the oldest. Unlike player
  state, they are kept across a lost connection.
- `ChatSync::SendLocalMessage` trims and validates the text. Being disconnected
  or a server rejection becomes a local notice line. `ChatSync::DrainLines`
  returns received messages and notices. Neither needs the game thread.
- `ChatWindow` keeps the latest 100 lines, each stamped with `ImGui::GetTime()`
  (seconds) when it arrived.

## Behavior

- Enter or keypad Enter during gameplay opens the chat and focuses its input.
  Typing goes only to the chat, and the character does not move or look around.
  Keys already held when it opened keep acting until released.
- Enter sends the text and closes the chat; Enter on an empty line only closes
  it. Escape discards the text typed since opening and closes it. Every key,
  the console and menu keys included, goes to the chat while it is open, but a
  menu that opens by itself, such as dialogue started by an NPC or a loading
  screen, closes it and keeps the draft.
- While open, the chat sits in the lower-left corner above the magicka bar,
  30 font sizes wide with 10 lines of history. The mouse wheel scrolls 3 lines
  per notch, and Page Up/Down scroll a page. New lines scroll the history down
  unless it was scrolled back.
- While closed, the newest lines (at most 6) show for 10 seconds on a faint
  background and fade out over the last 2. With nothing recent, nothing is
  drawn.
- Sender names are gold, text is light grey, and notices are orange.

## Limitations

- Non-Latin text relies on the system fonts listed in
  `specs/010-ui-overlay.md`; a script none of them covers shows placeholders.
  Emoji and other characters outside the Basic Multilingual Plane cannot be
  typed, because Skyrim drops them before building `CharEvent`s.
- Letters used by editing shortcuts (Ctrl+A/C/V/X/Y/Z) follow physical QWERTY
  positions, not the keyboard layout.
- There is no mouse cursor, so text cannot be selected with the mouse.

## Verification

- Server: `test_app_chat.py` covers the relay, trimming, the empty name,
  malformed messages, the per-player rate limit, and forgetting it on
  disconnect. `test_protocol.py` covers `parse_chat_message`.
- In game (pending): Enter opens the chat only with no menu open; the character
  stands still while typing and W held from before opening keeps running until
  released; Enter sends and the line appears for both players; Escape closes
  without sending, and does not open the journal; a menu that opens by itself
  while typing closes the chat; recent lines fade after 10 seconds; the wheel
  and Page Up/Down scroll; stopping the server makes sending report a notice.
