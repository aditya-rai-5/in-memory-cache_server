#!/usr/bin/env python3
"""
Integration test client for the in-memory cache server.
Run the server first: ./bin/redis_like
Then run this script: python3 test_server.py
"""

import socket
import struct
import sys
import time

HOST = "127.0.0.1"
PORT = 6379

passed = 0
failed = 0


def send_command(sock: socket.socket, cmd: str) -> str:
    """Send a length-prefixed command and read the length-prefixed response."""
    payload = cmd.encode()
    sock.sendall(struct.pack(">I", len(payload)) + payload)
    return receive_response(sock)


def receive_response(sock: socket.socket) -> str:
    """Read one framed reply, including after a client half-closes its writes."""
    raw_len = b""
    while len(raw_len) < 4:
        chunk = sock.recv(4 - len(raw_len))
        if not chunk:
            raise ConnectionError("Server closed connection")
        raw_len += chunk

    resp_len = struct.unpack(">I", raw_len)[0]

    body = b""
    while len(body) < resp_len:
        chunk = sock.recv(resp_len - len(body))
        if not chunk:
            raise ConnectionError("Server closed connection mid-response")
        body += chunk

    return body.decode()


def check(label: str, got: str, expected: str):
    global passed, failed
    if got == expected:
        print(f"  PASS  {label}")
        passed += 1
    else:
        print(f"  FAIL  {label}")
        print(f"        expected: {expected!r}")
        print(f"        got:      {got!r}")
        failed += 1


def run_tests(s: socket.socket):
    print("\n=== PING ===")
    check("PING", send_command(s, "PING"), "PONG")
    check("ping (lowercase)", send_command(s, "ping"), "PONG")

    print("\n=== SET / GET ===")
    check("SET foo bar", send_command(s, "SET foo bar"), "OK")
    check("GET foo", send_command(s, "GET foo"), "bar")
    check("GET missing", send_command(s, "GET missing"), "(nil)")

    print("\n=== OVERWRITE ===")
    check("SET foo baz", send_command(s, "SET foo baz"), "OK")
    check("GET foo (overwritten)", send_command(s, "GET foo"), "baz")

    print("\n=== EXISTS ===")
    check("EXISTS foo (yes)", send_command(s, "EXISTS foo"), "1")
    check("EXISTS ghost (no)", send_command(s, "EXISTS ghost"), "0")

    print("\n=== DEL ===")
    check("DEL foo → 1", send_command(s, "DEL foo"), "1")
    check("DEL foo again → 0", send_command(s, "DEL foo"), "0")
    check("GET foo after DEL", send_command(s, "GET foo"), "(nil)")

    print("\n=== TTL (no expiry) ===")
    send_command(s, "SET permanent value")
    check("TTL permanent → -1", send_command(s, "TTL permanent"), "-1")
    check("TTL missing → -2", send_command(s, "TTL nokey"), "-2")

    print("\n=== SET EX ===")
    check("SET tmp val EX 5", send_command(s, "SET tmp val EX 5"), "OK")
    check("GET tmp (alive)", send_command(s, "GET tmp"), "val")
    ttl_val = int(send_command(s, "TTL tmp"))
    if 1 <= ttl_val <= 5:
        print(f"  PASS  TTL tmp in range [1,5] (got {ttl_val})")
        global passed; passed += 1
    else:
        print(f"  FAIL  TTL tmp expected 1-5, got {ttl_val}")
        global failed; failed += 1

    print("\n=== SET EX expiry (waiting 3s...) ===")
    send_command(s, "SET shortlived x EX 2")
    time.sleep(3)
    check("GET shortlived (expired)", send_command(s, "GET shortlived"), "(nil)")
    check("TTL shortlived (expired)", send_command(s, "TTL shortlived"), "-2")

    print("\n=== Error handling ===")
    check("GET (no args)", send_command(s, "GET"), "ERR wrong number of arguments for 'GET' command")
    check("SET (1 arg)", send_command(s, "SET onlykey"), "ERR wrong number of arguments for 'SET' command")
    check("SET EX bad value", send_command(s, "SET k v EX notanumber"), "ERR value is not an integer or out of range")
    check("SET EX negative", send_command(s, "SET k v EX -5"), "ERR invalid expire time in 'SET' command")
    check("UNKNOWN command", send_command(s, "BLAH xyz"), "ERR unknown command")

    print("\n=== Strict command syntax ===")
    check("PING rejects arguments", send_command(s, "PING extra"),
          "ERR wrong number of arguments for 'PING' command")
    check("SET validation fixture", send_command(s, "SET strict original EX 600"), "OK")
    original_ttl = int(send_command(s, "TTL strict"))
    validation_started = time.monotonic()
    invalid_sets = [
        ("SET strict changed EX", "ERR wrong number of arguments for 'SET' command"),
        ("SET strict changed extra", "ERR wrong number of arguments for 'SET' command"),
        ("SET strict changed EX 5 extra", "ERR wrong number of arguments for 'SET' command"),
        ("SET strict changed PX 5", "ERR syntax error"),
        ("SET strict changed EX 5abc", "ERR value is not an integer or out of range"),
        ("SET strict changed EX 5.0", "ERR value is not an integer or out of range"),
        ("SET strict changed EX +5", "ERR value is not an integer or out of range"),
        ("SET strict changed EX 9223372036854775808", "ERR value is not an integer or out of range"),
        ("SET strict changed EX 9223372036854775807", "ERR value is not an integer or out of range"),
        ("SET strict changed EX 0", "ERR invalid expire time in 'SET' command"),
    ]
    for command, expected in invalid_sets:
        check(command, send_command(s, command), expected)
        check("rejected SET preserves value", send_command(s, "GET strict"), "original")
    elapsed = int(time.monotonic() - validation_started)
    current_ttl = int(send_command(s, "TTL strict"))
    check("rejected SET preserves TTL",
          str(original_ttl - elapsed - 1 <= current_ttl <= original_ttl), "True")
    check("mixed-case EX", send_command(s, "sEt strict updated eX 600"), "OK")
    check("valid SET after rejected commands", send_command(s, "GET strict"), "updated")
    check("plain SET removes expiry", send_command(s, "SET strict permanent"), "OK")
    check("TTL removed", send_command(s, "TTL strict"), "-1")
    check("DEL validation fixture", send_command(s, "DEL strict"), "1")

    print("\n=== Case insensitivity ===")
    send_command(s, "set mykey myval")
    check("set (lowercase)", send_command(s, "get mykey"), "myval")
    check("EXISTS (mixed case)", send_command(s, "Exists mykey"), "1")
    check("DEL (uppercase)", send_command(s, "DEL mykey"), "1")


def run_network_tests(control: socket.socket):
    address = control.getpeername()
    print("\n=== Socket lifecycle and server survival ===")

    with socket.create_connection(address, timeout=5) as peer:
        packet = struct.pack(">I", 4) + b"PING"
        peer.sendall(packet + packet)
        peer.shutdown(socket.SHUT_WR)
        check("half-close preserves first reply", receive_response(peer), "PONG")
        check("half-close preserves second reply", receive_response(peer), "PONG")
        check("server closes after replies drain", repr(peer.recv(1)), "b''")

    with socket.create_connection(address, timeout=5) as peer:
        peer.sendall(struct.pack(">I", 4) + b"PI")
        peer.shutdown(socket.SHUT_WR)
        check("incomplete frame at EOF closes connection", repr(peer.recv(1)), "b''")
    check("server alive after incomplete frame", send_command(control, "PING"), "PONG")

    with socket.create_connection(address, timeout=5) as peer:
        peer.sendall(struct.pack(">I", 1024 * 1024 + 1))
        check("oversized frame closes connection", repr(peer.recv(1)), "b''")
    check("server alive after protocol error", send_command(control, "PING"), "PONG")

    value = "x" * (256 * 1024)
    check("reset test fixture", send_command(control, "SET network_large " + value), "OK")
    request = b"GET network_large"
    packet = struct.pack(">I", len(request)) + request
    # Each peer requests more reply data than a typical socket send buffer,
    # then resets without reading. This exercises both read and write failures.
    for _ in range(5):
        with socket.create_connection(address, timeout=5) as peer:
            peer.sendall(packet * 16)
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        check("server survives reset with pending replies", send_command(control, "PING"), "PONG")
    check("remove reset test fixture", send_command(control, "DEL network_large"), "1")

    for _ in range(20):
        with socket.create_connection(address, timeout=5):
            pass
    with socket.create_connection(address, timeout=5) as peer:
        check("new client works after repeated disconnects", send_command(peer, "PING"), "PONG")
    check("existing client survives other disconnects", send_command(control, "PING"), "PONG")


if __name__ == "__main__":
    try:
        with socket.create_connection((HOST, PORT), timeout=5) as s:
            run_tests(s)
            run_network_tests(s)
    except ConnectionRefusedError:
        print(f"\nERROR: Could not connect to {HOST}:{PORT}")
        print("Make sure the server is running:  ./bin/redis_like")
        sys.exit(1)

    print(f"\n{'─' * 40}")
    print(f"Results: {passed} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)
