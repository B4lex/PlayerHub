# The admin panel is static files and Go cross-compiles, so both build stages run on the
# build host and only the final stage runs per target platform.
FROM --platform=$BUILDPLATFORM node:22-alpine AS admin-ui
WORKDIR /src
COPY admin-ui/package.json admin-ui/package-lock.json ./
RUN npm ci
COPY admin-ui/ ./
RUN npm run build

FROM --platform=$BUILDPLATFORM golang:1.26-alpine AS server
ARG TARGETOS TARGETARCH
WORKDIR /src
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server/ ./
# modernc.org/sqlite is pure Go, so the binary needs no C library.
RUN CGO_ENABLED=0 GOOS=$TARGETOS GOARCH=$TARGETARCH go build -trimpath -ldflags="-s -w" -o /out/server ./cmd/server

FROM alpine:3.22
RUN adduser -D -H -u 10001 playerhub && mkdir /data && chown playerhub /data
COPY --from=server /out/server /usr/local/bin/playerhub-server
COPY --from=admin-ui /src/dist /app/admin-ui
USER playerhub
# The session database and recordings default to paths relative to the working directory.
WORKDIR /data
VOLUME /data
ENTRYPOINT ["playerhub-server"]
