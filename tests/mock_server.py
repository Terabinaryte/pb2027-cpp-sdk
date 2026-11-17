#!/usr/bin/env python3
"""Minimal TLS WebSocket echo server used by the M0 transport smoke test.

Python standard library only. Usage:
    python3 tests/mock_server.py --port 9443 --cert tests/tls/cert.pem --key tests/tls/key.pem

It performs the RFC 6455 handshake, echoes binary/text frames, answers ping with
pong and closes on close. This is deliberately small: M2 will grow it into a
scripted protocol server with RULES/TICK/PUBLIC.
"""

import argparse
import base64
import hashlib
import socket
import ssl
import struct
import sys
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(conn, n):
    chunks = []
    remaining = n
    while remaining:
        chunk = conn.recv(remaining)
        if not chunk:
            raise ConnectionError("peer closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame(conn):
    b1, b2 = recv_exact(conn, 2)
    fin = bool(b1 & 0x80)
    opcode = b1 & 0x0F
    masked = bool(b2 & 0x80)
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack("!H", recv_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", recv_exact(conn, 8))[0]
    mask = recv_exact(conn, 4) if masked else b""
    payload = recv_exact(conn, length)
    if masked:
        payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    return fin, opcode, payload


def encode_frame(opcode, payload=b""):
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([n])
    elif n < 65536:
        header += bytes([126]) + struct.pack("!H", n)
    else:
        header += bytes([127]) + struct.pack("!Q", n)
    return header + payload


def handshake(conn):
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        request += chunk
        if len(request) > 65536:
            return False

    lines = request.split(b"\r\n")
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    key = headers.get(b"sec-websocket-key")
    if key is None:
        return False

    accept = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest())
    response = (
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n"
    )
    conn.sendall(response)
    return True


def serve_conn(conn, addr):
    try:
        if not handshake(conn):
            return
        while True:
            fin, opcode, payload = read_frame(conn)
            if opcode == 0x8:  # close
                conn.sendall(encode_frame(0x8, payload[:2]))
                return
            if opcode == 0x9:  # ping
                conn.sendall(encode_frame(0xA, payload))
                continue
            if opcode == 0xA:  # pong
                continue
            # echo text/binary
            conn.sendall(encode_frame(opcode, payload))
    except (ConnectionError, OSError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9443)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    args = parser.parse_args()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.key)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((args.host, args.port))
        s.listen(8)
        print(f"READY {args.host}:{args.port}", flush=True)
        while True:
            raw, addr = s.accept()
            try:
                conn = ctx.wrap_socket(raw, server_side=True)
            except ssl.SSLError:
                raw.close()
                continue
            threading.Thread(target=serve_conn, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
