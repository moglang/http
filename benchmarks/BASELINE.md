# Local v0.1.0 smoke baseline

Recorded 2026-08-09 with a Debug Mog/runtime and package build on Ubuntu 24.04,
Linux 6.17, x86_64, AMD Ryzen 7 7730U (8 cores/16 threads). The client and server
ran on loopback. HTTP samples open one connection per request; 200 operations
were measured per request/message case.

| Case | Throughput | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: |
| Plain-text GET | 6,558 req/s | 0.139 ms | 0.182 ms | 0.224 ms |
| Small JSON | 6,704 req/s | 0.147 ms | 0.167 ms | 0.178 ms |
| Text WebSocket echo | 19,240 msg/s | 0.049 ms | 0.074 ms | 0.096 ms |
| Binary WebSocket echo | 16,141 msg/s | 0.060 ms | 0.079 ms | 0.090 ms |

| Idle WebSockets | Resident memory |
| ---: | ---: |
| 1 | 13.6 MiB |
| 100 | 13.8 MiB |
| 1,000 | 15.3 MiB |

The local run was capped at 1,000 idle connections; Mog exposes no
portable allocation counter (`allocation_count` is `null`). These are
non-gating smoke numbers for regression orientation, not comparisons with raw
uWebSockets or another runtime.
