#!/usr/bin/env sh
# Regenerates the checked-in Go and nanopb code from proto/. Run it on the host
# (Linux or Windows with Git Bash), never under wine. Needs Go and uv.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
tools=$(mktemp -d)
trap 'rm -rf "$tools"' EXIT

# protoc-gen-go at the version server/go.mod pins.
(cd "$root/server" && GOBIN="$tools" go install google.golang.org/protobuf/cmd/protoc-gen-go)

go_out="$root/server/internal/wire/pb"
nanopb_out="$root/src/network/proto"
rm -rf "$go_out" "$nanopb_out"
mkdir -p "$go_out" "$nanopb_out"

protos="skyrimmp/v1/state.proto skyrimmp/v1/messages.proto"
# The pinned grpcio-tools has wheels for these Python versions only; on a newer
# one uv would try to compile it.
python=">=3.9,<3.14"

cd "$root/proto"
uv run --no-project --quiet --python "$python" --with grpcio-tools==1.71.0 python -m grpc_tools.protoc \
    -I . --plugin=protoc-gen-go="$tools/protoc-gen-go" \
    --go_out="$go_out" --go_opt=paths=source_relative $protos
mv "$go_out"/skyrimmp/v1/*.go "$go_out"
rm -rf "$go_out/skyrimmp"

uv run --no-project --quiet --python "$python" --with nanopb==0.4.9 --with grpcio-tools==1.71.0 nanopb_generator \
    -I . -D "$nanopb_out" $protos
