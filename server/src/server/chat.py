"""Per-player rate limit for chat messages."""

import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field

CHAT_BURST_MESSAGES = 5
CHAT_BURST_WINDOW_SECONDS = 5.0


@dataclass
class ChatThrottle:
    """Allows each player at most ``limit`` messages in any ``window_seconds``."""

    limit: int = CHAT_BURST_MESSAGES
    window_seconds: float = CHAT_BURST_WINDOW_SECONDS
    clock: Callable[[], float] = time.monotonic
    _sent: dict[str, deque[float]] = field(default_factory=dict)

    def allow(self, player_id: str) -> bool:
        now = self.clock()
        sent = self._sent.setdefault(player_id, deque())
        while sent and now - sent[0] >= self.window_seconds:
            sent.popleft()
        if len(sent) >= self.limit:
            return False
        sent.append(now)
        return True

    def forget(self, player_id: str) -> None:
        self._sent.pop(player_id, None)
