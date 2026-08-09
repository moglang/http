# Changelog

## 0.1.0

- Add complete HTTP route, request snapshot, body-limit, response helper,
  validation, abort, stale-handle, automatic-204, and contained-500 behavior.
- Add WebSocket upgrade/open/text/binary/drain/close callbacks, three-state send
  results, UTF-8 and close validation, payload/backpressure/idle controls,
  pub/sub, rooted per-socket data, and shutdown invalidation.
- Vendor pinned uWebSockets and uSockets sources for offline builds.
- Add Linux x86_64, Linux ARM64, and macOS ARM64 CI plus standard-library-only
  HTTP/WebSocket integration, GC stress, and sanitizer coverage.
- Ensure HEAD requests handled by `any` suppress body bytes, parse only
  segment-leading route parameters, and explicitly invalidate retained active
  response and WebSocket states during shutdown.
- Expand lifecycle, abort, body-limit, query/header, fragmentation,
  backpressure/drain, binary publication, UTF-8, close-validation, and live
  shutdown integration coverage.
