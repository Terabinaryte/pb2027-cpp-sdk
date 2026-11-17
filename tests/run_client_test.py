#!/usr/bin/env python3
"""Run the M2 client test against a scripted TLS protocol server."""

import os
import socket
import subprocess
import sys
import threading
import time


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def ensure_cert():
    cert = "tests/tls/cert.pem"
    key = "tests/tls/key.pem"
    if os.path.exists(cert) and os.path.exists(key):
        return cert, key
    os.makedirs("tests/tls", exist_ok=True)
    subprocess.check_call(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", key, "-out", cert, "-days", "2",
            "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return cert, key


def main():
    if len(sys.argv) != 2:
        print("usage: run_client_test.py <test-executable>", file=sys.stderr)
        return 2

    cert, key = ensure_cert()
    port = free_port()
    server = subprocess.Popen(
        [
            sys.executable, "tests/protocol_server.py",
            "--port", str(port), "--cert", cert, "--key", key,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    ready = False
    for _ in range(50):
        line = server.stdout.readline()
        if line.startswith("READY"):
            ready = True
            break
        if not line:
            break
    # Drain remaining server output so the pipe never fills.
    threading.Thread(target=lambda: server.stdout.read(), daemon=True).start()

    if not ready:
        server.terminate()
        print("server did not become ready", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["PB2027_TEST_PORT"] = str(port)
    try:
        rc = subprocess.call([sys.argv[1]], env=env, cwd=os.getcwd())
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
    return rc


if __name__ == "__main__":
    sys.exit(main())
