# Mog HTTP

Native single-threaded HTTP and WebSocket server support for Mog, backed by
uWebSockets and uSockets.

Milestone 1 provides server lifecycle, GET and HEAD routing, copied request
metadata, staged status/header state, and terminal text responses. Request
bodies and the remainder of the HTTP/WebSocket v1 API arrive in later
milestones.

```mog
const http = @import("github.com/moglang/http")

const server http.Server = http.createServer()
http.get(server, "/hello", fn(req http.Request, res http.Response) void {
    http.text(res, "Hello from Mog")
})

if (!http.listen(server, "127.0.0.1", 3000)) {
    error("could not bind HTTP server")
}
http.run(server)
```

`run` blocks on the VM thread. Call `stop` from a handler to close the listener
and allow active responses to drain before the loop exits.

The package vendors uWebSockets and uSockets and builds without TLS or zlib.
See `vendor/THIRD_PARTY.md` for pinned revisions and license details.
