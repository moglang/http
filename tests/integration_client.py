#!/usr/bin/env python3

from __future__ import annotations

import os
import base64
import hashlib
from pathlib import Path
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import traceback


def mog_command(mog: Path, *arguments: str) -> list[str]:
    command = [str(mog), *arguments]
    if os.environ.get("MOG_HTTP_VALGRIND") == "1":
        return [
            "valgrind", "--quiet", "--leak-check=full",
            "--errors-for-leak-kinds=definite", "--error-exitcode=99",
            *command,
        ]
    return command


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


def request(
    port: int,
    method: str,
    target: str,
    headers: list[tuple[str, str]] | None = None,
    body: bytes = b"",
    split_body: bool = False,
) -> tuple[int, dict[str, str], bytes]:
    header_text = (
        f"{method} {target} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Connection: close\r\n"
    )
    supplied = list(headers or [])
    if body and not any(name.lower() == "content-length" for name, _ in supplied):
        supplied.append(("Content-Length", str(len(body))))
    for name, value in supplied:
        header_text += f"{name}: {value}\r\n"
    payload = (header_text + "\r\n").encode("latin-1")
    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
        client.sendall(payload)
        if split_body and len(body) > 1:
            middle = len(body) // 2
            client.sendall(body[:middle])
            time.sleep(0.01)
            client.sendall(body[middle:])
        elif body:
            client.sendall(body)
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


def recv_exact(connection: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise RuntimeError("WebSocket closed during frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def websocket_frame(opcode: int, payload: bytes, *, final: bool = True) -> bytes:
    first = (0x80 if final else 0) | opcode
    mask = b"\x12\x34\x56\x78"
    length = len(payload)
    if length < 126:
        header = bytes((first, 0x80 | length))
    elif length <= 0xFFFF:
        header = bytes((first, 0x80 | 126)) + length.to_bytes(2, "big")
    else:
        header = bytes((first, 0x80 | 127)) + length.to_bytes(8, "big")
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return header + mask + masked


def read_websocket_frame(connection: socket.socket) -> tuple[int, bytes]:
    first, second = recv_exact(connection, 2)
    opcode = first & 0x0F
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(recv_exact(connection, 2), "big")
    elif length == 127:
        length = int.from_bytes(recv_exact(connection, 8), "big")
    mask = recv_exact(connection, 4) if second & 0x80 else b""
    payload = recv_exact(connection, length)
    if mask:
        payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    return opcode, payload


def websocket_connect(port: int, target: str = "/ws", *, expect_open: bool = True) -> socket.socket:
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    key = base64.b64encode(b"mog-http-test-key").decode("ascii")
    request_bytes = (
        f"GET {target} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode("ascii")
    connection.sendall(request_bytes)
    response = b""
    while b"\r\n\r\n" not in response:
        response += recv_exact(connection, 1)
    if not response.startswith(b"HTTP/1.1 101"):
        connection.close()
        raise RuntimeError(f"WebSocket upgrade failed: {response!r}")
    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
    )
    if b"sec-websocket-accept: " + expected.lower() not in response.lower():
        connection.close()
        raise RuntimeError("WebSocket accept key mismatch")
    if expect_open:
        opcode, payload = read_websocket_frame(connection)
        if opcode != 1 or payload != b"open":
            connection.close()
            raise RuntimeError(f"unexpected open frame: {opcode}, {payload!r}")
    return connection


def websocket_send(connection: socket.socket, opcode: int, payload: bytes) -> None:
    connection.sendall(websocket_frame(opcode, payload))


def websocket_expect(connection: socket.socket, opcode: int, payload: bytes) -> None:
    actual_opcode, actual_payload = read_websocket_frame(connection)
    if (actual_opcode, actual_payload) != (opcode, payload):
        raise RuntimeError(
            f"unexpected WebSocket frame: {(actual_opcode, actual_payload)!r}, "
            f"expected {(opcode, payload)!r}"
        )


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


def run_source_case(
    mog: Path, staged: Path, root: Path, name: str, source: str
) -> subprocess.CompletedProcess[str]:
    project = root / name
    project.mkdir()
    (project / "case.mog").write_text(source, encoding="utf-8")
    (project / "mog.toml").write_text(
        'kind = "project"\n'
        f'name = "{name}"\n'
        'version = "0.0.0"\n\n'
        f'[dependencies]\n"github.com/moglang/http" = {{ path = "{staged}", version = "0.1.0" }}\n',
        encoding="utf-8",
    )
    environment = os.environ.copy()
    environment["MOG_CACHE_DIR"] = str(root / f"{name}-cache")
    return subprocess.run(
        mog_command(mog, "run", "--offline", "case.mog"), cwd=project,
        env=environment, capture_output=True, text=True, timeout=20,
    )


def run_example_smokes(mog: Path, package: Path, staged: Path, root: Path) -> None:
    for name, marker in (
        ("http_server", "Listening on"),
        ("websocket_echo", "WebSocket echo listening"),
    ):
        port = free_port()
        project = root / f"example-{name}"
        project.mkdir()
        source = (package / "examples" / f"{name}.mog").read_text(encoding="utf-8")
        (project / "example.mog").write_text(source.replace("3000", str(port)), encoding="utf-8")
        (project / "mog.toml").write_text(
            'kind = "project"\n'
            f'name = "example-{name}"\n'
            'version = "0.0.0"\n\n'
            f'[dependencies]\n"github.com/moglang/http" = {{ path = "{staged}", version = "0.1.0" }}\n',
            encoding="utf-8",
        )
        environment = os.environ.copy()
        environment["MOG_CACHE_DIR"] = str(root / f"example-{name}-cache")
        process = subprocess.Popen(
            mog_command(mog, "run", "--offline", "example.mog"), cwd=project,
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            wait_for_line(process, marker, 30)
            if name == "http_server":
                status, _, body = request(port, "GET", "/hello")
                if status != 200 or body != b"Hello from Mog":
                    fail(f"HTTP example smoke failed: {status}, {body!r}", process)
                request(port, "GET", "/stop")
                process.wait(timeout=10)
            else:
                connection = websocket_connect(port, "/echo", expect_open=False)
                websocket_send(connection, 1, b"example")
                websocket_expect(connection, 1, b"example")
                connection.close()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
def run_full_suite(mog: Path, package: Path, library: Path) -> None:
    sample = package / "tests" / "samples" / "full_server.mog"
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="mog-http-full-") as temporary:
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
                    'name = "http-full-integration"',
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
            mog_command(mog, "run", "--offline", "server.mog"),
            cwd=project,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_line(process, "HTTP_FULL_READY", 30)

            binary_body = b"a\x00\xff\nz"
            status, headers, body = request(
                port,
                "POST",
                "/echo/42?q=hello+world&empty=&first=one&first=two&encoded%20key=value%2Fok&bare",
                [("X-Test", "first"), ("x-test", "second")],
                binary_body,
                split_body=True,
            )
            if status != 200 or body != binary_body:
                fail(f"HTTP body/metadata response failed: {status}, {headers}, {body!r}", process)
            if headers.get("content-type") != "application/octet-stream":
                fail(f"binary content type missing: {headers}", process)
            if not headers.get("x-remote"):
                fail(f"remote address missing: {headers}", process)

            status, _, body = request(port, "GET", "/get-body", body=b"get-body")
            if status != 200 or body != b"get-body":
                fail(f"GET request body failed: {status}, {body!r}", process)

            aborted = socket.create_connection(("127.0.0.1", port), timeout=3)
            aborted.sendall(
                f"POST /abort HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Length: 10\r\n\r\nabc".encode("ascii")
            )
            aborted.close()
            time.sleep(0.02)
            status, _, body = request(port, "GET", "/abort-count")
            if status != 200 or body != b"0":
                fail(f"aborted request invoked handler: {status}, {body!r}", process)

            status, _, body = request(port, "GET", "/saved")
            if status != 200 or body != binary_body:
                fail(f"retained request body snapshot failed: {status}, {body!r}", process)

            status, _, body = request(port, "GET", "/stale")
            if status != 500 or body != b"Internal Server Error":
                fail(f"stale response safety failed: {status}, {body!r}", process)

            for method in ("PUT", "PATCH", "DELETE", "OPTIONS"):
                status, _, body = request(port, method, "/method")
                if status != 200 or body != method.encode("ascii"):
                    fail(f"{method} route failed: {status}, {body!r}", process)
            status, _, body = request(port, "TRACE", "/any")
            if status != 200 or body != b"TRACE":
                fail(f"any route failed: {status}, {body!r}", process)
            status, headers, body = request(port, "HEAD", "/any")
            if status != 200 or body or headers.get("content-length") != "4":
                fail(f"HEAD any fallback failed: {status}, {headers}, {body!r}", process)
            status, _, body = request(port, "GET", "/literal:colon")
            if status != 200 or body != b"literal":
                fail(f"literal colon route failed: {status}, {body!r}", process)
            status, _, body = request(port, "GET", "/empty-header", [("X-Empty", "")])
            if status != 200 or body != b"empty":
                fail(f"present-empty header failed: {status}, {body!r}", process)

            status, headers, body = request(port, "GET", "/json")
            if status != 200 or body != b"true" or headers.get("content-type") != "application/json; charset=utf-8":
                fail(f"JSON response failed: {status}, {headers}, {body!r}", process)
            status, headers, body = request(port, "GET", "/redirect")
            if status != 307 or body or headers.get("location") != "/json":
                fail(f"redirect failed: {status}, {headers}, {body!r}", process)
            status, _, body = request(port, "GET", "/end")
            if status != 202 or body:
                fail(f"empty response failed: {status}, {body!r}", process)
            status, _, body = request(port, "GET", "/completed-error")
            if status != 200 or body != b"already sent":
                fail(f"post-completion error policy failed: {status}, {body!r}", process)
            for target in ("/bad-framing", "/bad-injection", "/bad-status", "/bad-status-body", "/bad-redirect", "/lifecycle-error"):
                status, _, body = request(port, "GET", target)
                if status != 500 or body != b"Internal Server Error":
                    fail(f"validation route {target} failed: {status}, {body!r}", process)
            status, _, body = request(port, "GET", "/bad-query?value=%GG")
            if status != 200 or body != b"null":
                fail(f"malformed query behavior failed: {status}, {body!r}", process)
            status, headers, body = request(port, "GET", "/custom-content-type")
            if status != 200 or body != b"custom" or headers.get("content-type") != "application/x-mog":
                fail(f"custom content type was not preserved: {status}, {headers}, {body!r}", process)
            status, _, _ = request(
                port,
                "POST",
                "/echo/42?q=hello+world&empty=&first=one&first=two&encoded%20key=value%2Fok&bare",
                [("X-Test", "first")],
                b"x" * 65,
                split_body=True,
            )
            if status != 413:
                fail(f"body limit failed: {status}", process)

            first = websocket_connect(port)
            websocket_send(first, 1, b"hello")
            websocket_expect(first, 1, b"hello")
            first.sendall(websocket_frame(1, b"frag", final=False))
            first.sendall(websocket_frame(0, b"mented"))
            websocket_expect(first, 1, b"fragmented")
            websocket_send(first, 2, bytes(range(256)))
            websocket_expect(first, 2, bytes(range(256)))
            websocket_send(first, 1, b"data")
            websocket_expect(first, 1, b"data-ok")
            websocket_send(first, 1, b"replace-data")
            websocket_expect(first, 1, b"replace-ok")
            websocket_send(first, 9, b"ping")
            websocket_expect(first, 10, b"ping")
            websocket_send(first, 1, b"subscribe")
            websocket_expect(first, 1, b"subscribed")

            second = websocket_connect(port)
            websocket_send(second, 1, b"subscribe")
            websocket_expect(second, 1, b"subscribed")
            websocket_send(first, 1, b"publish")
            websocket_expect(first, 1, b"published")
            websocket_expect(second, 1, b"broadcast")
            websocket_send(first, 1, b"publish-binary")
            websocket_expect(first, 1, b"binary-published")
            websocket_expect(second, 2, bytes((0, 127, 255)))
            websocket_send(second, 1, b"unsubscribe")
            websocket_expect(second, 1, b"unsubscribed")
            websocket_send(second, 8, (1000).to_bytes(2, "big") + b"bye")
            close_opcode, close_payload = read_websocket_frame(second)
            if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 1000:
                fail(f"WebSocket close handshake failed: {close_opcode}, {close_payload!r}", process)
            second.close()

            websocket_send(first, 1, b"error")
            close_opcode, close_payload = read_websocket_frame(first)
            if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 1011 or close_payload[2:] != b"handler error":
                fail(f"WebSocket handler error policy failed: {close_opcode}, {close_payload!r}", process)
            first.close()

            invalid_text = websocket_connect(port)
            websocket_send(invalid_text, 1, b"invalid-text")
            close_opcode, close_payload = read_websocket_frame(invalid_text)
            if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 1011:
                fail(f"invalid outbound UTF-8 was accepted: {close_opcode}, {close_payload!r}", process)
            invalid_text.close()

            invalid_close = websocket_connect(port)
            websocket_send(invalid_close, 1, b"invalid-close")
            close_opcode, close_payload = read_websocket_frame(invalid_close)
            if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 1011:
                fail(f"reserved close code was accepted: {close_opcode}, {close_payload!r}", process)
            invalid_close.close()

            for command in (b"long-close", b"invalid-close-utf8"):
                invalid_reason = websocket_connect(port)
                websocket_send(invalid_reason, 1, command)
                close_opcode, close_payload = read_websocket_frame(invalid_reason)
                if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 1011:
                    fail(f"invalid close reason was accepted: {command!r}, {close_payload!r}", process)
                invalid_reason.close()

            open_error = websocket_connect(port, "/ws-open-error", expect_open=False)
            close_opcode, close_payload = read_websocket_frame(open_error)
            if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 1011:
                fail(f"open callback failure was not contained: {close_opcode}, {close_payload!r}", process)
            open_error.close()

            if os.environ.get("MOG_HTTP_VALGRIND") != "1":
                pressure = websocket_connect(port, "/ws-pressure", expect_open=False)
                time.sleep(0.05)
                pressure.settimeout(10)
                pressure_ok = False
                for _ in range(600):
                    opcode, payload = read_websocket_frame(pressure)
                    if opcode == 1 and payload == b"pressure-ok":
                        pressure_ok = True
                        break
                    if opcode == 8:
                        break
                if not pressure_ok:
                    fail("backpressure/drain scenario did not report all send outcomes", process)
                websocket_send(pressure, 8, (1000).to_bytes(2, "big"))
                try:
                    read_websocket_frame(pressure)
                finally:
                    pressure.close()

            nested = websocket_connect(port)
            websocket_send(nested, 1, b"close-now")
            close_opcode, close_payload = read_websocket_frame(nested)
            if close_opcode != 8 or int.from_bytes(close_payload[:2], "big") != 4000 or close_payload[2:] != b"nested":
                fail(f"nested close callback failed: {close_opcode}, {close_payload!r}", process)
            nested.close()

            status, _, body = request(port, "GET", "/closed-safe")
            if status != 200 or body != b"closed-ok":
                fail(f"retained closed socket state failed: {status}, {body!r}", process)
            status, _, body = request(port, "GET", "/send-closed")
            if status != 500 or body != b"Internal Server Error":
                fail(f"send-after-close safety failed: {status}, {body!r}", process)

            oversized = websocket_connect(port)
            websocket_send(oversized, 2, b"x" * 65537)
            try:
                close_opcode, _ = read_websocket_frame(oversized)
                if close_opcode != 8:
                    fail(f"WebSocket payload limit returned opcode {close_opcode}", process)
            except (ConnectionResetError, RuntimeError):
                # uWebSockets may terminate an oversized peer without a close frame.
                pass
            oversized.close()

            for _ in range(int(os.environ.get("MOG_HTTP_CHURN_COUNT", "1000"))):
                churn = websocket_connect(port)
                websocket_send(churn, 8, (1000).to_bytes(2, "big"))
                try:
                    read_websocket_frame(churn)
                finally:
                    churn.close()

            shutdown_socket = websocket_connect(port)
            status, _, body = request(port, "GET", "/stop")
            if status != 200 or body != b"stopping":
                fail(f"full-suite stop failed: {status}, {body!r}", process)
            try:
                shutdown_socket.recv(1)
            except (ConnectionResetError, OSError):
                pass
            finally:
                shutdown_socket.close()
            process.wait(timeout=10)
            if process.returncode != 0:
                fail("full server exited unsuccessfully", process)
            assert process.stdout is not None
            if "HTTP_FULL_STOPPED" not in process.stdout.read():
                fail("full server run did not return", process)
        except Exception as error:
            fail(str(error), process)

    print("HTTP/WebSocket full integration passed")


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
            mog_command(mog, "run", "--offline", "server.mog"),
            cwd=project,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_line(process, "HTTP_MILESTONE1_READY", 30)

            stop_before_run = run_source_case(
                mog, staged, root, "stop-before-run",
                'const http = @import("github.com/moglang/http")\n'
                'const server http.Server = http.createServer()\n'
                'http.stop(server)\nhttp.stop(server)\nhttp.run(server)\nprint("STOP_BEFORE_RUN_OK")\n',
            )
            if stop_before_run.returncode != 0 or "STOP_BEFORE_RUN_OK" not in stop_before_run.stdout:
                fail(f"stop-before-run failed: {stop_before_run.stdout}\n{stop_before_run.stderr}", process)

            occupied = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            occupied_port = occupied.getsockname()[1]
            try:
                bind_failure = run_source_case(
                    mog, staged, root, "bind-failure",
                    'const http = @import("github.com/moglang/http")\n'
                    'const server http.Server = http.createServer()\n'
                    f'if (http.listen(server, "127.0.0.1", {occupied_port}i64)) {{ error("unexpected bind") }}\n'
                    'print("BIND_FAILURE_OK")\n',
                )
            finally:
                occupied.close()
            if bind_failure.returncode != 0 or "BIND_FAILURE_OK" not in bind_failure.stdout:
                fail(f"bind-failure behavior failed: {bind_failure.stdout}\n{bind_failure.stderr}", process)

            duplicate_route = run_source_case(
                mog, staged, root, "duplicate-route",
                'const http = @import("github.com/moglang/http")\n'
                'const server http.Server = http.createServer()\n'
                'http.get(server, "/:id/:id", fn(req http.Request, res http.Response) void { http.end(res) })\n',
            )
            if duplicate_route.returncode == 0 or "duplicate parameter" not in duplicate_route.stderr:
                fail(f"duplicate route parameter was accepted: {duplicate_route.stdout}\n{duplicate_route.stderr}", process)

            immutable_route = run_source_case(
                mog, staged, root, "immutable-route",
                'const http = @import("github.com/moglang/http")\n'
                'const server http.Server = http.createServer()\n'
                'const route http.WebSocketRoute = http.createWebSocketRoute()\n'
                'http.websocket(server, "/ws", route)\n'
                'http.setIdleTimeout(route, 8i64)\n',
            )
            if immutable_route.returncode == 0 or "immutable" not in immutable_route.stderr:
                fail(f"registered WebSocket route remained mutable: {immutable_route.stdout}\n{immutable_route.stderr}", process)

            finalizer_stress = run_source_case(
                mog, staged, root, "server-finalizers",
                'const http = @import("github.com/moglang/http")\n'
                'var index i64 = 0i64\nwhile (index < 1000i64) {\n'
                '  const server http.Server = http.createServer()\n  index = index + 1i64\n}\n'
                'print("SERVER_FINALIZERS_OK")\n',
            )
            if finalizer_stress.returncode != 0 or "SERVER_FINALIZERS_OK" not in finalizer_stress.stdout:
                fail(f"server finalizer stress failed: {finalizer_stress.stdout}\n{finalizer_stress.stderr}", process)

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

            status, _, _ = request(port, "POST", "/default-limit", body=b"x" * (1024 * 1024 + 1))
            if status != 413:
                fail(f"default body limit or handler suppression failed: {status}", process)

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
            run_example_smokes(mog, package, staged, root)
        except Exception:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            raise

    print("HTTP Milestone 1 integration passed")
    run_full_suite(mog, package, library)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        traceback.print_exc()
        print(error, file=sys.stderr)
        raise SystemExit(1)
