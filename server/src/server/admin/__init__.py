"""Server admin panel backend: REST commands, live state push, and UI hosting."""

from server.admin.api import create_admin_router
from server.admin.broadcaster import StateBroadcaster
from server.admin.socket import publish_state, register_admin_namespace
from server.admin.state import AdminContext, build_state
from server.admin.static import mount_admin_ui

__all__ = [
    "AdminContext",
    "StateBroadcaster",
    "build_state",
    "create_admin_router",
    "mount_admin_ui",
    "publish_state",
    "register_admin_namespace",
]
