#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def fail(message: str, process: subprocess.Popen[str] | None = None) -> None:
    if process is not None:
        process.kill()
        stdout, stderr = process.communicate(timeout=5)
        message += f"\nserver stdout:\n{stdout}\nserver stderr:\n{stderr}"
    raise RuntimeError(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def request(port: int, method: str, target: str) -> tuple[int, dict[str, str], bytes]:
    payload = (
        f"{method} {target} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode("ascii")
    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
        client.sendall(payload)
        chunks: list[bytes] = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    response = b"".join(chunks)
    head, separator, body = response.partition(b"\r\n\r\n")
    if not separator:
        raise RuntimeError(f"malformed HTTP response: {response!r}")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    headers: dict[str, str] = {}
    for line in lines[1:]:
        name, value = line.split(":", 1)
        headers[name.lower()] = value.strip()
    return status, headers, body


def wait_for_line(process: subprocess.Popen[str], marker: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    assert process.stdout is not None
    while time.monotonic() < deadline:
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if ready:
            line = process.stdout.readline()
            if marker in line:
                return
        if process.poll() is not None:
            fail(f"server exited before {marker}", process)
    fail(f"timed out waiting for {marker}", process)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: integration_client.py <mog> <package-dir> <package.so>", file=sys.stderr)
        return 2

    mog = Path(sys.argv[1]).resolve()
    package = Path(sys.argv[2]).resolve()
    library = Path(sys.argv[3]).resolve()
    sample = package / "tests" / "samples" / "milestone1_server.mog"
    port = free_port()

    with tempfile.TemporaryDirectory(prefix="mog-http-m1-") as temporary:
        root = Path(temporary)
        staged = root / "github.com" / "moglang" / "http"
        project = root / "project"
        cache = root / "cache"
        shutil.copytree(package, staged, ignore=shutil.ignore_patterns("build", ".git"))
        library_name = "package.dylib" if sys.platform == "darwin" else "package.so"
        shutil.copy2(library, staged / library_name)
        project.mkdir()
        source = sample.read_text(encoding="utf-8").replace("__PORT__", str(port) + "i64")
        (project / "server.mog").write_text(source, encoding="utf-8")
        (project / "mog.toml").write_text(
            "\n".join(
                [
                    'kind = "project"',
                    'name = "http-milestone1-integration"',
                    'version = "0.0.0"',
                    "",
                    "[dependencies]",
                    f'"github.com/moglang/http" = {{ path = "{staged}", version = "0.1.0" }}',
                    "",
                ]
            ),
            encoding="utf-8",
        )

        environment = os.environ.copy()
        environment["MOG_CACHE_DIR"] = str(cache)
        process = subprocess.Popen(
            [str(mog), "run", "--offline", "server.mog"],
            cwd=project,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_line(process, "HTTP_MILESTONE1_READY", 30)

            status, headers, body = request(port, "GET", "/hello?source=integration")
            if status != 201 or body != b"hello":
                fail(f"unexpected GET response: {status}, {headers}, {body!r}", process)
            if headers.get("x-milestone") != "one":
                fail(f"staged/replaced header missing: {headers}", process)
            if headers.get("content-type") != "text/plain; charset=utf-8":
                fail(f"default text content type missing: {headers}", process)

            status, _, body = request(port, "GET", "/snapshot")
            if status != 200 or body != b"/hello":
                fail(f"copied request snapshot failed: {status}, {body!r}", process)

            status, headers, body = request(port, "GET", "/error")
            if status != 500 or body != b"Internal Server Error" or "x-discarded" in headers:
                fail(f"handler error containment failed: {status}, {headers}, {body!r}", process)

            status, _, body = request(port, "GET", "/hello?source=integration")
            if status != 201 or body != b"hello":
                fail("server did not recover after callback error", process)

            status, _, body = request(port, "GET", "/empty")
            if status != 204 or body:
                fail(f"automatic 204 failed: {status}, {body!r}", process)

            status, headers, body = request(port, "HEAD", "/head")
            if status != 200 or body or headers.get("content-length") != "9":
                fail(f"HEAD response failed: {status}, {headers}, {body!r}", process)

            status, _, body = request(port, "GET", "/stop")
            if status != 200 or body != b"stopping":
                fail(f"stop response failed: {status}, {body!r}", process)

            process.wait(timeout=10)
            if process.returncode != 0:
                fail("server exited unsuccessfully", process)
            assert process.stdout is not None
            remaining_stdout = process.stdout.read()
            if "HTTP_MILESTONE1_STOPPED" not in remaining_stdout:
                fail("run did not return after callback stop", process)
        except Exception:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            raise

    print("HTTP Milestone 1 integration passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
