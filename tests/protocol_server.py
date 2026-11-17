#!/usr/bin/env python3
"""Scripted pb2027 protocol server for M2 client tests.

Behaves like a minimal /ws endpoint:
  * sends RULES on connect;
  * sends one Ping and requires a Pong;
  * decodes BATCH, replies with one TICK (results + brushes + sonars);
  * returns COOLING for pixel 777 so the client error path is exercised;
  * echoes close frames.

Python standard library only (3.14+ for compression.zstd).
"""

import argparse
import base64
import hashlib
import socket
import ssl
import struct
import sys
import threading
import time
from compression import zstd

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
COOLING = 1
OK = 0


def recv_exact(conn, n):
    chunks = []
    while n:
        chunk = conn.recv(n)
        if not chunk:
            raise ConnectionError("peer closed")
        chunks.append(chunk)
        n -= len(chunk)
    return b"".join(chunks)


def read_frame(conn):
    b1, b2 = recv_exact(conn, 2)
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
    return opcode, payload


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


def pad4(n):
    return (n + 3) & ~3


def put16(buf, off, value):
    struct.pack_into("<H", buf, off, value)


def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value)


def put64(buf, off, value):
    struct.pack_into("<Q", buf, off, value)


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def rules_payload():
    buf = bytearray(96)
    buf[0] = 0x84
    buf[1] = 1
    put16(buf, 4, 1000)
    put16(buf, 6, 600)
    put16(buf, 8, 2)
    put32(buf, 12, 128)
    put64(buf, 16, 1_000_000)
    put64(buf, 24, 100_000)
    put64(buf, 32, 60_000)
    put64(buf, 40, 1_000)
    put32(buf, 64, 8000)
    put32(buf, 68, 65536)
    put32(buf, 72, 128)
    put32(buf, 76, 128)
    put32(buf, 80, 1 << 20)
    put32(buf, 84, 1 << 20)
    put64(buf, 88, time.time_ns() // 1000)
    return bytes(buf)


def parse_batch(payload):
    if payload[0] != 0x01 or payload[1] != 1:
        raise ValueError("not a BATCH")
    nb = u32(payload, 4)
    np = u32(payload, 8)
    bind_slot = 16
    bind_token = bind_slot + pad4(2 * nb)
    paint_slot = bind_token + 16 * nb
    paint_idx = paint_slot + pad4(2 * np)
    paint_color = paint_idx + 4 * np
    expected = paint_color + 4 * np
    if len(payload) != expected:
        raise ValueError(f"BATCH length mismatch {len(payload)} != {expected}")

    binds = []
    for i in range(nb):
        slot = u16(payload, bind_slot + 2 * i)
        token = payload[bind_token + 16 * i: bind_token + 16 * (i + 1)]
        binds.append((slot, token))
    paints = []
    for i in range(np):
        slot = u16(payload, paint_slot + 2 * i)
        idx = u32(payload, paint_idx + 4 * i)
        color = u32(payload, paint_color + 4 * i)
        paints.append((slot, idx, color))
    return binds, paints


def build_tick(tick, results, brushes, sonars, lost=()):
    """results: list of (status, value); brushes: list of (slot, t, idx);
    sonars: list of (center, [13 colors])."""
    nr = len(results)
    nb = len(brushes)
    nk = len(sonars)
    ns = len(lost)
    radius = 2
    a = 2 * radius * radius + 2 * radius + 1

    buf = bytearray()
    buf += bytes([0x81, 1])
    buf += struct.pack("<H", radius)
    buf += struct.pack("<I", nr)
    buf += struct.pack("<Q", tick)
    buf += struct.pack("<III", nb, nk, ns)
    buf += struct.pack("<I", 0)

    for _, t, _ in brushes:
        buf += struct.pack("<Q", t)
    for _, _, idx in brushes:
        buf += struct.pack("<I", idx)
    for slot, _, _ in brushes:
        buf += struct.pack("<H", slot)
    buf += b"\x00" * (pad4(2 * nb) - 2 * nb)

    for status, _ in results:
        buf += bytes([status])
    buf += b"\x00" * (pad4(nr) - nr)

    for _, value in results:
        buf += struct.pack("<I", value)
    for center, _ in sonars:
        buf += struct.pack("<I", center)
    for _, colors in sonars:
        for color in colors:
            buf += struct.pack("<I", color)

    for slot in lost:
        buf += struct.pack("<H", slot)
    buf += b"\x00" * (pad4(2 * ns) - 2 * ns)
    return bytes(buf)


def handshake(conn):
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        request += chunk
    headers = {}
    for line in request.split(b"\r\n")[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().lower()] = v.strip()
    key = headers.get(b"sec-websocket-key")
    if key is None:
        return False
    accept = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest())
    conn.sendall(
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n"
    )
    return True


def send_message(conn, payload):
    conn.sendall(encode_frame(0x2, zstd.compress(payload)))


def serve(conn, addr):
    print(f"CONNECT {addr}", flush=True)
    tick = 0
    got_pong = False
    try:
        conn.settimeout(5.0)
        if not handshake(conn):
            return
        send_message(conn, rules_payload())
        conn.sendall(encode_frame(0x9, b"pb2027-ping"))  # Ping
        print("PING_SENT", flush=True)

        while True:
            opcode, payload = read_frame(conn)
            if opcode == 0x8:
                conn.sendall(encode_frame(0x8, payload[:2]))
                return
            if opcode == 0xA:
                got_pong = True
                print("PONG", flush=True)
                continue
            if opcode == 0x9:
                conn.sendall(encode_frame(0xA, payload))
                continue
            if opcode != 0x2:
                continue

            binds, paints = parse_batch(zstd.decompress(payload))
            results = []
            brush = {}
            sonars = []

            for slot, token in binds:
                results.append((OK, 0))
                brush[slot] = (0, 0)  # empty brush after bind
                print(f"BIND slot={slot} token={token.hex()}", flush=True)

            now_us = time.time_ns() // 1000
            lost_slots = []
            for slot, idx, color in paints:
                if idx == 999:
                    print("DROP_CONNECTION", flush=True)
                    return
                if idx == 888:
                    conn.sendall(encode_frame(0x8, struct.pack("!H", 4000) + b"test-close"))
                    print("CLOSE_SENT 4000", flush=True)
                    return
                if idx == 666:
                    results.append((OK, 0))
                    lost_slots.append(slot)
                    print(f"PAINT slot={slot} idx={idx} LOST", flush=True)
                    continue
                if idx == 777:
                    results.append((COOLING, 123456))
                    print(f"PAINT slot={slot} idx={idx} COOLING", flush=True)
                    continue
                results.append((OK, 0))
                brush[slot] = (now_us, idx)
                colors = [0xFF000000 | ((idx + i) & 0xFFFFFF) for i in range(13)]
                sonars.append((idx, colors))
                print(f"PAINT slot={slot} idx={idx} color={color:08x}", flush=True)

            brushes = [(slot, brush[slot][0], brush[slot][1]) for slot in sorted(brush)]
            tick += 1
            send_message(conn, build_tick(tick, results, brushes, sonars, lost_slots))
            print(f"TICK {tick} results={len(results)}", flush=True)
    except (ConnectionError, OSError, ValueError, ssl.SSLError) as e:
        print(f"CONNECTION_END {e}", flush=True)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9444)
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
            threading.Thread(target=serve, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
