"""Serves the built admin UI (server/admin-ui) at /admin."""

from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import HTMLResponse
from starlette.exceptions import HTTPException
from starlette.staticfiles import StaticFiles
from starlette.types import Scope

ADMIN_UI_DIRECTORY = Path(__file__).resolve().parents[3] / "admin-ui"
ADMIN_UI_BUILD = ADMIN_UI_DIRECTORY / "dist"
ADMIN_URL_PREFIX = "/admin"

_NOT_BUILT = """<!doctype html>
<title>Admin panel not built</title>
<body style="font: 16px system-ui; max-width: 40rem; margin: 4rem auto; padding: 0 1rem">
<h1>Admin panel not built</h1>
<p>Build it once with <code>npm install &amp;&amp; npm run build</code> in
<code>server/admin-ui</code>, or run <code>npm run dev</code> there and open the
address it prints. The REST API is available at <a href="/docs">/docs</a> meanwhile.</p>
</body>"""


class SinglePageApp(StaticFiles):
    """Static files that answer unknown paths with index.html, for client-side routes."""

    async def get_response(self, path: str, scope: Scope):
        try:
            return await super().get_response(path, scope)
        except HTTPException as error:
            if error.status_code != 404 or Path(path).suffix:
                raise
            return await super().get_response("index.html", scope)


def mount_admin_ui(app: FastAPI, build_directory: Path = ADMIN_UI_BUILD) -> None:
    if (build_directory / "index.html").is_file():
        app.mount(
            ADMIN_URL_PREFIX,
            SinglePageApp(directory=build_directory, html=True),
            name="admin-ui",
        )
        return

    @app.get(ADMIN_URL_PREFIX, include_in_schema=False)
    @app.get(f"{ADMIN_URL_PREFIX}/{{path:path}}", include_in_schema=False)
    async def admin_not_built(path: str = "") -> HTMLResponse:
        del path
        return HTMLResponse(_NOT_BUILT, status_code=503)
