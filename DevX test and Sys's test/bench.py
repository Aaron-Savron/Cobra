#!/usr/bin/env python3
"""
Concurrent HTTP benchmark client for the Cobra vs Python web engine shootout.

Each request opens a fresh TCP connection (the servers force Connection: close),
which mirrors how the engines actually work. The client measures client-visible
latency: connect + request + response + close.

Usage:
    python3 bench.py <host> <port> <path> [total_requests] [concurrency]
"""
import argparse
import socket
import statistics
import threading
import time


def one_request(host, port, path, method="GET", body=None):
    s = socket.create_connection((host, port), timeout=30)
    try:
        req = f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n"
        if body is not None:
            req += f"Content-Length: {len(body)}\r\n"
        req += "Connection: close\r\n\r\n"
        s.sendall(req.encode())
        if body is not None:
            s.sendall(body)
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        status = int(head.split(b" ")[1])
        return status, len(rest)
    finally:
        s.close()


def worker(host, port, path, count, results, idx):
    for _ in range(count):
        t0 = time.perf_counter()
        try:
            status, body_len = one_request(host, port, path)
            dt = (time.perf_counter() - t0) * 1e3
            results[idx].append((status, body_len, dt))
        except Exception:
            results[idx].append((-1, 0, -1.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("path")
    ap.add_argument("total", type=int, nargs="?", default=2000)
    ap.add_argument("concurrency", type=int, nargs="?", default=16)
    args = ap.parse_args()

    per = args.total // args.concurrency
    results = [[] for _ in range(args.concurrency)]
    threads = []
    t0 = time.perf_counter()
    for i in range(args.concurrency):
        t = threading.Thread(target=worker, args=(args.host, args.port, args.path, per, results, i))
        threads.append(t)
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    flat = [r for group in results for r in group]
    ok = [r for r in flat if r[0] == 200]
    errors = len(flat) - len(ok)
    lats = sorted(r[2] for r in ok)

    def pct(p):
        if not lats:
            return 0.0
        idx = min(len(lats) - 1, int(len(lats) * p))
        return lats[idx]

    n = len(flat)
    rate = n / elapsed
    print(f"target : {args.host}:{args.port}{args.path}")
    print(f"requests: {n} (concurrency {args.concurrency})  errors: {errors}")
    print(f"total  : {elapsed:.2f}s   throughput: {rate:.0f} req/s")
    if lats:
        print(f"latency (ms): avg {statistics.mean(lats):.2f}  "
              f"median {statistics.median(lats):.2f}  p95 {pct(0.95):.2f}  p99 {pct(0.99):.2f}  "
              f"min {min(lats):.2f}  max {max(lats):.2f}")
    avg_body = statistics.mean(r[1] for r in ok) if ok else 0
    print(f"avg body : {avg_body:.0f} bytes")


if __name__ == "__main__":
    main()
