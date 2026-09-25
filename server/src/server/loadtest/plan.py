"""Where each bot of a batch starts in the shared recording, and how fast it plays."""

import random
from dataclasses import dataclass

from server.playback.session import MAXIMUM_RATE, MINIMUM_RATE

MAXIMUM_BOTS = 500
MAXIMUM_RAMP_UP_SECONDS = 600.0
MAXIMUM_RATE_SPREAD = 0.5


@dataclass(frozen=True)
class BotPlan:
    index: int
    start_ms: int
    rate: float
    join_delay_seconds: float


def plan_bots(
    count: int,
    duration_ms: int,
    *,
    seed: int,
    jitter: float = 0.5,
    rate_spread: float = 0.0,
    ramp_up_seconds: float = 0.0,
) -> list[BotPlan]:
    """Spread ``count`` bots evenly over the recording's timeline.

    Bot ``i`` is placed at ``i * duration / count`` and moved by up to
    ``jitter / 2`` of that spacing either way, so neighbours never swap places.
    ``rate_spread`` varies each bot's speed around 1x, which makes the spacing
    drift over time instead of repeating exactly on every loop.

    A bot that joins during the ramp-up starts that much further along, so the
    bots keep their spacing as if they had all started together. Offsets and
    rates come from separate generators, so changing one setting keeps the
    other's layout for the same seed.
    """
    offsets = random.Random(f"{seed}:offset")
    rates = random.Random(f"{seed}:rate")
    slot_ms = duration_ms / count
    plans: list[BotPlan] = []
    for index in range(count):
        rate = 1.0 + rates.uniform(-rate_spread, rate_spread)
        rate = round(min(max(rate, MINIMUM_RATE), MAXIMUM_RATE), 3)
        join_delay = ramp_up_seconds * index / count
        offset_ms = (index + offsets.uniform(-0.5, 0.5) * jitter) * slot_ms
        start_ms = offset_ms + join_delay * 1000.0 * rate
        plans.append(
            BotPlan(
                index=index,
                start_ms=int(start_ms % duration_ms) if duration_ms > 0 else 0,
                rate=rate,
                join_delay_seconds=join_delay,
            )
        )
    return plans
