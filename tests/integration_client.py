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


def websocket_frame(opcode: int, payload: bytes) -> bytes:
    first = 0x80 | opcode
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


def websocket_connect(port: int, target: str = "/ws") -> socket.socket:
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
            [str(mog), "run", "--offline", "server.mog"],
            cwd=project,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            wait_for_line(process, "HTTP_FULL_READY", 30)

            binary_body = b"a\x00\xffz"
            status, headers, body = request(
                port,
                "POST",
                "/echo/42?q=hello+world&empty=",
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
            for target in ("/bad-framing", "/bad-status-body", "/bad-redirect"):
                status, _, body = request(port, "GET", target)
                if status != 500 or body != b"Internal Server Error":
                    fail(f"validation route {target} failed: {status}, {body!r}", process)
            status, _, body = request(port, "GET", "/bad-query?value=%GG")
            if status != 200 or body != b"null":
                fail(f"malformed query behavior failed: {status}, {body!r}", process)
            status, _, _ = request(
                port,
                "POST",
                "/echo/42?q=hello+world&empty=",
                [("X-Test", "first")],
                b"x" * 65,
                split_body=True,
            )
            if status != 413:
                fail(f"body limit failed: {status}", process)

            first = websocket_connect(port)
            websocket_send(first, 1, b"hello")
            websocket_expect(first, 1, b"hello")
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

            for _ in range(1000):
                churn = websocket_connect(port)
                websocket_send(churn, 8, (1000).to_bytes(2, "big"))
                try:
                    read_websocket_frame(churn)
                finally:
                    churn.close()

            status, _, body = request(port, "GET", "/stop")
            if status != 200 or body != b"stopping":
                fail(f"full-suite stop failed: {status}, {body!r}", process)
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
    run_full_suite(mog, package, library)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        traceback.print_exc()
        print(error, file=sys.stderr)
        raise SystemExit(1)
