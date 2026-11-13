#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: package-server.sh

Build the release server for linux/amd64 and windows/amd64 and the admin
panel into build/dist/playerhub-server-<version>.zip. Its linux/ and windows/
directories each hold a complete server: the binary, the panel, a config.toml
that lists every key at its default, the licenses, and a README.txt.

The version comes from set_version in xmake.lua.
EOF
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(sed -n 's/^set_version("\([^"]*\)").*/\1/p' "$root/xmake.lua")"
[[ -n "$version" ]] || { echo "No set_version in xmake.lua" >&2; exit 1; }
targets=(linux/amd64 windows/amd64)
archive="playerhub-server-$version"

out="$root/build/dist"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Building admin panel"
(
    cd "$root/admin-ui"
    [[ -d node_modules ]] || npm ci
    npm run build
)

# Every key with its default, from the configuration block in server/README.md,
# with the panel taken from the archive's own admin-ui directory.
config="$work/config.toml"
{
    echo "# PlayerHub server $version. Every key is listed at its default; delete"
    echo "# or change any of them. Unknown keys are an error."
    echo "# Run from this directory: playerhub-server --config config.toml"
    echo
    awk '/^## Configuration/ { section = 1 } section && /^```toml/ { inside = 1; next }
         inside && /^```/ { exit } inside { print }' "$root/server/README.md" |
        sed 's|^admin_ui_directory = "[^"]*"|admin_ui_directory = "admin-ui"|'
} > "$config"
grep -q '^admin_ui_directory = "admin-ui"' "$config" ||
    { echo "Configuration block not found in server/README.md" >&2; exit 1; }

# Licenses of everything linked into the binary or bundled into the panel.
licenses="$work/licenses"
mkdir -p "$licenses"
copy_license() {
    local name="$1" dir="$2" file
    for file in "$dir"/LICENSE* "$dir"/LICENCE* "$dir"/COPYING* "$dir"/NOTICE*; do
        if [[ -f "$file" ]]; then cp "$file" "$licenses/${name//\//_}.$(basename "$file")"; fi
    done
}
for package in react react-dom react-router react-router-dom scheduler cookie set-cookie-parser; do
    if [[ -d "$root/admin-ui/node_modules/$package" ]]; then
        copy_license "npm/$package" "$root/admin-ui/node_modules/$package"
    fi
done

mkdir -p "$out"
for target in "${targets[@]}"; do
    os="${target%/*}"
    arch="${target#*/}"
    stage="$work/$archive/$os"
    binary="playerhub-server"
    if [[ "$os" == windows ]]; then binary+=".exe"; fi
    echo "Building $os/$arch"
    mkdir -p "$stage"

    # modernc.org/sqlite is pure Go, so the binary needs no C library.
    (
        cd "$root/server"
        CGO_ENABLED=0 GOOS="$os" GOARCH="$arch" \
            go build -trimpath -ldflags="-s -w" -o "$stage/$binary" ./cmd/server
        GOOS="$os" GOARCH="$arch" go list -deps -f '{{with .Module}}{{if not .Main}}{{.Path}} {{.Dir}}{{end}}{{end}}' ./cmd/server |
            sort -u > "$work/modules-$os-$arch"
    )
    mkdir -p "$stage/licenses"
    cp "$licenses"/* "$stage/licenses/" 2>/dev/null || true
    while read -r module dir; do
        for file in "$dir"/LICENSE* "$dir"/LICENCE* "$dir"/COPYING* "$dir"/NOTICE* "$dir"/PATENTS*; do
            if [[ -f "$file" ]]; then cp "$file" "$stage/licenses/go_${module//\//_}.$(basename "$file")"; fi
        done
    done < "$work/modules-$os-$arch"
    # The standard library is linked in too; distribution packages such as
    # Fedora's move its license out of GOROOT.
    for dir in "$(go env GOROOT)" /usr/share/licenses/golang; do
        if [[ -f "$dir/LICENSE" ]]; then
            cp "$dir/LICENSE" "$stage/licenses/go_stdlib.LICENSE"
            if [[ -f "$dir/PATENTS" ]]; then cp "$dir/PATENTS" "$stage/licenses/go_stdlib.PATENTS"; fi
            break
        fi
    done
    [[ -f "$stage/licenses/go_stdlib.LICENSE" ]] || { echo "Go license not found" >&2; exit 1; }

    cp -r "$root/admin-ui/dist" "$stage/admin-ui"
    cp "$config" "$stage/config.toml"
    cp "$root/LICENSE" "$root/EXCEPTIONS" "$stage/"

    if [[ "$os" == windows ]]; then
        run=".\\$binary --config config.toml"
    else
        run="./$binary --config config.toml"
    fi
    cat > "$stage/README.txt" <<EOF
PlayerHub server $version ($os/$arch)

Run it from this directory:

    $run

Players connect on port 3000 (listen). The admin panel is at
http://127.0.0.1:3001/admin/ (admin_listen) and has no authentication: keep
that address off the public Internet. The session database (sessions.db) and
recordings/ are created in the working directory.

Every setting is in config.toml at its default. To let players on other
computers connect, allow inbound TCP port 3000 through the firewall; behind a
reverse proxy on the same host, set listen = "127.0.0.1:3000" and add the
proxy's address to trusted_proxies.

PlayerHub is licensed under GPL-3.0-or-later with the exceptions in
EXCEPTIONS; licenses of the libraries it includes are in licenses/.
EOF
done

rm -f "$out/$archive.zip"
(cd "$work" && zip -qr -X "$out/$archive.zip" "$archive")
echo "Wrote $out/$archive.zip"
