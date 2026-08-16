#!/usr/bin/env python3
"""
Python equivalent of the Cobra web engine.

Same routes, same protocol (HTTP/1.1 with Connection: close), same
single-threaded accept loop as web_engine.cb, so the benchmark is
apples-to-apples. A threaded mode is included because that is how Python
servers are usually deployed in practice.

Usage:
    python3 python_server.py [port] [threaded]
"""
import os
import socket
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18101
THREADED = len(sys.argv) > 2 and sys.argv[2] == "threaded"

SITE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "site", "index.html")


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def build_json_records(count):
    parts = []
    for i in range(count):
        score = (i * 37 + 5) / 10.0
        active = "true" if i % 2 == 0 else "false"
        parts.append('{"id":%d,"name":"item-%d","score":%.1f,"active":%s}' % (i, i, score, active))
    return '{"count":%d,"data":[%s]}' % (count, ",".join(parts))


def read_site():
    try:
        with open(SITE_PATH, "rb") as f:
            return f.read()
    except OSError:
        return None


def parse_query(target, name, fallback):
    if "?" not in target:
        return fallback
    query = target.split("?", 1)[1]
    for pair in query.split("&"):
        if pair.startswith(name + "="):
            try:
                return int(pair.split("=", 1)[1])
            except ValueError:
                return fallback
    return fallback


def route(method, target, body):
    start = time.monotonic()
    us = lambda: int((time.monotonic() - start) * 1e6)  # noqa: E731

    if method == "GET":
        if target == "/api/ping" or target.startswith("/api/ping?"):
            return 200, "application/json", '{"endpoint":"ping","engine":"python","us":%d}' % us()
        if target == "/api/fib" or target.startswith("/api/fib?"):
            n = parse_query(target, "n", 28)
            f0 = time.monotonic()
            result = fib(n)
            fib_us = int((time.monotonic() - f0) * 1e6)
            return 200, "application/json", '{"endpoint":"fib","n":%d,"result":%d,"fib_us":%d,"us":%d}' % (n, result, fib_us, us())
        if target == "/api/stress" or target.startswith("/api/stress?"):
            n = parse_query(target, "n", 200000)
            total = 0
            for i in range(n):
                total += i * i
            return 200, "application/json", '{"endpoint":"stress","n":%d,"result":%d,"us":%d}' % (n, total, us())
        if target == "/api/json" or target.startswith("/api/json?"):
            n = min(parse_query(target, "n", 100), 1000)
            return 200, "application/json", build_json_records(n)
        if target == "/" or target == "/site" or target.startswith("/site?"):
            data = read_site()
            if data is None:
                return 404, "application/json", '{"error":"no index file"}'
            return 200, "text/html", data
        return 404, "application/json", '{"error":"not found"}'
    if method == "POST":
        if target == "/api/echo" or target.startswith("/api/echo?"):
            received = len(body)
            hexdump = body[:16].hex()
            return 200, "application/json", '{"method":"POST","received":%d,"hex":"%s","us":%d}' % (received, hexdump, us())
        return 404, "application/json", '{"error":"not found"}'
    return 404, "application/json", '{"error":"method not allowed"}'


def handle(client):
    try:
        data = client.recv(8192)
        if not data:
            return
        head, _, body = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        method = parts[0] if len(parts) > 0 else ""
        target = parts[1] if len(parts) > 1 else "/"
        status, ctype, payload = route(method, target, body)
        if isinstance(payload, str):
            payload = payload.encode()
        header = (
            "HTTP/1.1 %d OK\r\nConnection: close\r\nContent-Type: %s\r\n"
            "Content-Length: %d\r\n\r\n" % (status, ctype, len(payload))
        )
        client.sendall(header.encode() + payload)
    except OSError:
        pass
    finally:
        try:
            client.close()
        except OSError:
            pass


def serve(server):
    while True:
        client, _ = server.accept()
        if THREADED:
            threading.Thread(target=handle, args=(client,), daemon=True).start()
        else:
            handle(client)


def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", PORT))
    server.listen(128)
    mode = "threaded" if THREADED else "single-threaded"
    print(f"python web engine ({mode}) listening on {PORT}", flush=True)
    serve(server)


if __name__ == "__main__":
    main()
