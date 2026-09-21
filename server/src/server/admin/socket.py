"""The /admin Socket.IO namespace: one-way live state for admin panels.

Commands go through the REST API; the namespace only carries ``state``
snapshots to the server's admin panels. It is separate from the default
namespace, so a panel is never counted as a player.
"""

from typing import Any

import socketio

from server.admin.broadcaster import StateBroadcaster

ADMIN_NAMESPACE = "/admin"
STATE_EVENT = "state"


def register_admin_namespace(
    sio: socketio.AsyncServer,
    broadcaster: StateBroadcaster,
    build_state: Any,
) -> None:
    async def connect(sid: str, environ: dict[str, Any], auth: Any = None) -> None:
        del environ, auth
        broadcaster.listener_added()
        await sio.emit(STATE_EVENT, build_state(), to=sid, namespace=ADMIN_NAMESPACE)

    async def disconnect(sid: str, reason: str) -> None:
        del sid, reason
        broadcaster.listener_removed()

    sio.on("connect", connect, namespace=ADMIN_NAMESPACE)
    sio.on("disconnect", disconnect, namespace=ADMIN_NAMESPACE)


async def publish_state(sio: socketio.AsyncServer, state: Any) -> None:
    await sio.emit(STATE_EVENT, state, namespace=ADMIN_NAMESPACE)
