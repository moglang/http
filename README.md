# Mog HTTP

Production-oriented, single-threaded HTTP/1.1 and WebSocket server support for
Mog, backed by uWebSockets 20.77.0 and uSockets.

## Install

```sh
mog add github.com/moglang/http@v0.1.0
```

```mog
const http = @import("github.com/moglang/http")

const server http.Server = http.createServer()
http.get(server, "/users/:id", fn(req http.Request, res http.Response) void {
    const id str? = http.param(req, "id")
    if (id == null) {
        http.status(res, 400i64)
        http.text(res, "missing id")
        return
    }
    http.text(res, "User: " + id)
})

if (!http.listen(server, "0.0.0.0", 3000i64)) {
    error("could not bind HTTP server")
}
http.run(server)
```

`get`, `head`, `post`, `put`, `patch`, `delete`, `options`, and `any` register
routes. Request handles are owned snapshots: method, raw URL/path, headers,
decoded query values, named parameters, peer address, and body remain readable
after the callback. The default body limit is 1 MiB and can be changed with
`setMaxBodySize` before `listen`.

Responses stage status and headers until one terminal operation: `text`,
`bytes`, `json`, `redirect`, or `end`. Text, byte, and JSON responses default to
the appropriate content type. HEAD responses report the body length without
sending its bytes. An unfinished handler is completed as 204; a handler error
before completion becomes a generic 500. Response handles become inactive when
their callback ends, while `isCompleted` and `isAborted` remain safe.

## WebSockets

```mog
const route http.WebSocketRoute = http.createWebSocketRoute()

http.onOpen(route, fn(socket http.WebSocket, req http.Request) void {
    http.setSocketData(socket, "connected")
})
http.onText(route, fn(socket http.WebSocket, message str) void {
    const result i64 = http.sendText(socket, message)
    if (result == http.SEND_DROPPED) {
        print("message dropped")
    }
})
http.onBinary(route, fn(socket http.WebSocket, message Array<u8>) void {
    http.sendBinary(socket, message)
})
http.websocket(server, "/game", route)
```

The defaults are a 64 KiB incoming payload, 1 MiB backpressure limit,
close-on-limit enabled, 120-second idle timeout, automatic pings, and no
compression. Configure a route before registering it. Text and close reasons
must be valid UTF-8; binary messages use `Array<u8>` and preserve every byte.
Close reasons are limited to 123 bytes.

Send results are package constants:

- `SEND_SUCCESS`: accepted without additional user-space backpressure.
- `SEND_BACKPRESSURE`: accepted and buffered.
- `SEND_DROPPED`: not queued because the configured limit was already exceeded.

Sockets support subscribe/unsubscribe/membership and socket-scoped text or
binary publish. A publisher must subscribe to the topic, does not receive its
own publication, and `true` means at least one eligible subscriber was queued.
`setSocketData` retains any Mog value with exact object identity across garbage
collection; the root is released after the close callback. Handler failures in
open, message, or drain callbacks close the connection with 1011.

## Execution and lifetime model

`run` blocks the VM's owning OS thread. The VM, uWebSockets loop, and every
callback run synchronously on that one thread; there is no async/await or
background callback thread. Call `stop` before `run`, from a callback, or more
than once. Shutdown closes listeners and active sockets while callback roots are
still valid. v0.1.0 has no graceful OS-signal integration; Ctrl+C uses ordinary
process termination.

Calls that need a live socket fail after close begins. `isOpen` and
`bufferedAmount` remain safe on retained closed handles. Request snapshots stay
valid, but response mutation after its callback always fails safely.

## Build and support

Supported source-build targets are Linux x86_64 GNU, Linux ARM64 GNU, and macOS
ARM64. Building requires CMake 3.16+, a C11 compiler, and a C++17 compiler. The
vendored build is offline and uses epoll/kqueue with TLS, zlib, libuv, ASIO,
GCD, QUIC, and io_uring disabled.

The package is GPL-3.0-only. Vendored dependency revisions and their Apache-2.0
and MIT notices are documented in [`vendor/THIRD_PARTY.md`](vendor/THIRD_PARTY.md).
Run `tests/test_http_package.sh /path/to/mog .` for package validation and the
standard-library-only HTTP/WebSocket integration suite.
