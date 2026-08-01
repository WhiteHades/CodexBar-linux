#!/usr/bin/env python3

import concurrent.futures
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def fail(message):
    raise AssertionError(message)


def request(port, path, timeout=3):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    connection.request("GET", path)
    response = connection.getresponse()
    body = response.read().decode("utf-8")
    connection.close()
    return response.status, json.loads(body)


def write_config(path, source):
    path.write_text(
        json.dumps({"version": 1, "providers": [{"id": "codex", "enabled": True, "source": source}]}),
        encoding="utf-8",
    )


def read_count(path):
    return int(path.read_text(encoding="utf-8")) if path.exists() else 0


def wait_ready(process, port, error_path):
    for _ in range(80):
        try:
            if request(port, "/health", 0.25)[0] == 200:
                return
        except (ConnectionError, OSError):
            pass
        if process.poll() is not None:
            break
        time.sleep(0.05)
    fail(f"server did not become ready:\n{error_path.read_text(encoding='utf-8')}")


def main():
    if len(sys.argv) != 2:
        fail("usage: test_serve_concurrency.py BINARY")
    binary = str(Path(sys.argv[1]).resolve())
    work = Path.cwd() / f"codexbar-serve-concurrency.{os.getpid()}"
    work.mkdir()
    server = None
    slow_clients = []
    try:
        backend = work / "backend.py"
        backend.write_text(
            """#!/usr/bin/env python3
import fcntl
import json
import os
import time
from pathlib import Path

root = Path(os.environ["CODEXBAR_TEST_STATE"])
with (root / "count.lock").open("w") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    count_path = root / "count"
    count = int(count_path.read_text()) if count_path.exists() else 0
    count_path.write_text(str(count + 1))

active = (root / "active.lock").open("w")
try:
    fcntl.flock(active, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    with (root / "overlap").open("a") as output:
        output.write("overlap\\n")

mode = (root / "mode").read_text().strip()
if mode == "slow_a":
    time.sleep(0.3)
if mode == "timeout":
    time.sleep(3)

account = "other@example.test" if mode == "error_b" else "user@example.test"
if mode.startswith("error_"):
    row = {
        "provider": "codex",
        "account": account,
        "source": "test",
        "error": {"message": mode, "code": 1, "kind": "test"},
    }
else:
    updated = {
        "slow_a": "2026-01-01T00:00:00Z",
        "good_b": "2026-01-02T00:00:00Z",
        "good_c": "2026-01-03T00:00:00Z",
        "good_d": "2026-01-04T00:00:00Z",
        "good_e": "2026-01-05T00:00:00Z",
    }.get(mode, "2026-01-06T00:00:00Z")
    row = {
        "provider": "codex",
        "account": account,
        "source": "test",
        "note": mode,
        "usage": {"primary": {"usedPercent": 10}, "updatedAt": updated},
    }
print(json.dumps([row]))
""",
            encoding="utf-8",
        )
        backend.chmod(0o755)
        config = work / "config.json"
        mode = work / "mode"
        write_config(config, "oauth")
        mode.write_text("slow_a", encoding="utf-8")
        (work / "codex").mkdir()
        (work / "claude").mkdir()

        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        listener.close()
        output_path = work / "server.out"
        error_path = work / "server.err"
        output = output_path.open("w")
        error = error_path.open("w")
        environment = os.environ.copy()
        environment.update(
            {
                "CODEXBAR_BACKEND": str(backend),
                "CODEXBAR_CONFIG": str(config),
                "CODEXBAR_TEST_STATE": str(work),
                "CODEXBAR_COST_CODEX_ROOT": str(work / "codex"),
                "CODEXBAR_COST_CLAUDE_ROOT": str(work / "claude"),
            }
        )
        server = subprocess.Popen(
            [
                binary,
                "serve",
                "--port",
                str(port),
                "--refresh-interval",
                "0.1",
                "--request-timeout",
                "1",
            ],
            env=environment,
            stdout=output,
            stderr=error,
        )
        wait_ready(server, port, error_path)

        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
            responses = list(executor.map(lambda _: request(port, "/usage?provider=codex"), range(5)))
        if any(status != 200 or body[0].get("note") != "slow_a" for status, body in responses):
            fail(f"coalesced responses differed: {responses}")
        if read_count(work / "count") != 1:
            fail("same-fingerprint misses were not coalesced")

        mode.write_text("good_b", encoding="utf-8")
        write_config(config, "web")
        status, body = request(port, "/usage?provider=codex")
        if status != 200 or body[0].get("note") != "good_b" or read_count(work / "count") != 2:
            fail("a config fingerprint change reused the previous cache entry")

        time.sleep(0.15)
        mode.write_text("error_a", encoding="utf-8")
        status, body = request(port, "/usage?provider=codex")
        if status != 200 or body[0].get("note") != "good_b":
            fail(f"same-account last-good fallback failed: {status} {body}")
        if body[0]["usage"].get("updatedAt") != "2026-01-02T00:00:00Z":
            fail("last-good fallback did not preserve stale update metadata")

        mode.write_text("good_c", encoding="utf-8")
        status, body = request(port, "/usage?provider=codex")
        if status != 200 or body[0].get("note") != "good_c" or read_count(work / "count") != 4:
            fail("provider error response was cached")

        time.sleep(0.15)
        mode.write_text("error_b", encoding="utf-8")
        status, body = request(port, "/usage?provider=codex")
        if status != 200 or "error" not in body[0] or body[0].get("account") != "other@example.test":
            fail(f"last-good data crossed account boundaries: {status} {body}")
        mode.write_text("good_d", encoding="utf-8")
        status, body = request(port, "/usage?provider=codex")
        if status != 200 or body[0].get("note") != "good_d" or read_count(work / "count") != 6:
            fail("unmatched provider error response was cached")

        time.sleep(0.15)
        mode.write_text("timeout", encoding="utf-8")
        status, body = request(port, "/usage?provider=codex", 2)
        if status != 504 or body.get("error") != "request timed out":
            fail(f"provider timeout was not enforced: {status} {body}")
        mode.write_text("good_e", encoding="utf-8")
        status, body = request(port, "/usage?provider=codex", 3)
        if status != 200 or body[0].get("note") != "good_e":
            fail(f"request after timeout did not recover: {status} {body}")
        if (work / "overlap").exists():
            fail("timed-out provider work overlapped the following request")

        slow = socket.create_connection(("127.0.0.1", port), timeout=1)
        slow.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n")
        started = time.monotonic()
        status, _ = request(port, "/health", 1)
        if status != 200 or time.monotonic() - started >= 0.8:
            fail("a slow request reader serialized the server")
        slow.close()

        for _ in range(16):
            client = socket.create_connection(("127.0.0.1", port), timeout=1)
            client.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n")
            slow_clients.append(client)
        time.sleep(0.2)
        status, body = request(port, "/health", 1)
        if status != 503 or body.get("error") != "server busy":
            fail(f"pre-auth connection limit was not enforced: {status} {body}")
    finally:
        for client in slow_clients:
            client.close()
        if server is not None:
            server.terminate()
            try:
                server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
