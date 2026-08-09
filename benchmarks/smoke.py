#!/usr/bin/env python3
"""Non-gating correctness-oriented HTTP/WebSocket performance smoke runner."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from integration_client import (  # noqa: E402
    free_port,
    request,
    wait_for_line,
    websocket_connect,
    websocket_expect,
    websocket_send,
)


def percentiles(samples: list[float]) -> dict[str, float]:
    ordered = sorted(samples)
    def pick(percent: float) -> float:
        return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * percent))]
    return {"p50_ms": pick(0.50), "p95_ms": pick(0.95), "p99_ms": pick(0.99)}


def rss_bytes(process: subprocess.Popen[str]) -> int | None:
    status = Path(f"/proc/{process.pid}/status")
    if not status.exists():
        return None
    for line in status.read_text(encoding="utf-8").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return None


def main() -> int:
    if len(sys.argv) not in (4, 5, 6):
        print("usage: smoke.py <mog> <package-dir> <package-library> [iterations] [max-idle]", file=sys.stderr)
        return 2
    mog, package, library = map(lambda value: Path(value).resolve(), sys.argv[1:4])
    iterations = int(sys.argv[4]) if len(sys.argv) >= 5 else 200
    max_idle = int(sys.argv[5]) if len(sys.argv) >= 6 else 1000
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="mog-http-benchmark-") as temporary:
        root = Path(temporary)
        staged = root / "github.com" / "moglang" / "http"
        project = root / "project"
        shutil.copytree(package, staged, ignore=shutil.ignore_patterns("build", ".git", "__pycache__"))
        shutil.copy2(library, staged / ("package.dylib" if sys.platform == "darwin" else "package.so"))
        project.mkdir()
        source = (package / "benchmarks" / "benchmark_server.mog").read_text(encoding="utf-8")
        (project / "server.mog").write_text(source.replace("__PORT__", f"{port}i64"), encoding="utf-8")
        (project / "mog.toml").write_text(
            'kind = "project"\nname = "http-benchmark"\nversion = "0.0.0"\n\n'
            f'[dependencies]\n"github.com/moglang/http" = {{ path = "{staged}", version = "0.1.0" }}\n',
            encoding="utf-8",
        )
        environment = os.environ.copy()
        environment["MOG_CACHE_DIR"] = str(root / "cache")
        process = subprocess.Popen(
            [str(mog), "run", "--offline", "server.mog"], cwd=project,
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            wait_for_line(process, "HTTP_BENCHMARK_READY", 30)
            results: dict[str, object] = {"iterations": iterations, "allocation_count": None}
            for name, target in (("plaintext_get", "/plaintext"), ("small_json", "/json")):
                samples: list[float] = []
                started = time.perf_counter()
                for _ in range(iterations):
                    before = time.perf_counter()
                    status, _, _ = request(port, "GET", target)
                    if status != 200:
                        raise RuntimeError(f"{target} returned {status}")
                    samples.append((time.perf_counter() - before) * 1000)
                elapsed = time.perf_counter() - started
                results[name] = {"requests_per_second": iterations / elapsed, **percentiles(samples)}

            connection = websocket_connect(port, "/echo")
            for name, opcode, payload in (("text_echo", 1, b"hello"), ("binary_echo", 2, bytes(range(64)))):
                samples = []
                started = time.perf_counter()
                for _ in range(iterations):
                    before = time.perf_counter()
                    websocket_send(connection, opcode, payload)
                    websocket_expect(connection, opcode, payload)
                    samples.append((time.perf_counter() - before) * 1000)
                elapsed = time.perf_counter() - started
                results[name] = {"messages_per_second": iterations / elapsed, **percentiles(samples)}
            connection.close()

            idle: list[object] = []
            connections = []
            for target in (1, 100, 1000, 10000):
                if target > max_idle:
                    continue
                while len(connections) < target:
                    connections.append(websocket_connect(port, "/echo"))
                idle.append({"connections": target, "rss_bytes": rss_bytes(process)})
            results["idle_websockets"] = idle
            for connection in connections:
                connection.close()
            request(port, "GET", "/stop")
            process.wait(timeout=10)
            print(json.dumps(results, indent=2, sort_keys=True))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
