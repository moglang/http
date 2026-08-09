# Performance smoke test

After the correctness suite passes, build the package and run:

```sh
python3 benchmarks/smoke.py /path/to/mog . /path/to/package.so 1000 10000
```

The non-gating runner reports throughput and p50/p95/p99 latency for plain-text
GET, small JSON, and text/binary WebSocket echo. It also records resident memory
at 1, 100, 1,000, and (when requested) 10,000 idle WebSockets. Allocation count
is reported as unavailable because Mog does not currently expose a portable
allocation counter. Results describe this wrapper and client harness only; they
are not claims of equivalence with raw uWebSockets or another runtime.
