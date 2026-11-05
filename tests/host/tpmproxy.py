#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Unix-socket TPM proxy for tests: tpmproxy.py LISTEN UPSTREAM [retry=N] [split=K].
retry=N answers the first N commands with TPM_RC_RETRY (N=-1: always); split=K sends replies K bytes at a time."""
import os, socket, sys, time

listen, upstream = sys.argv[1], sys.argv[2]
opt = dict(a.split("=") for a in sys.argv[3:])
retry, split = int(opt.get("retry", 0)), int(opt.get("split", 0))
RETRY = bytes.fromhex("80010000000a00000922")

def read_cmd(c):
    h = b""
    while len(h) < 10:
        d = c.recv(10 - len(h))
        if not d: return None
        h += d
    n = int.from_bytes(h[2:6], "big")
    while len(h) < n:
        h += c.recv(n - len(h))
    return h

def read_reply(u):
    h = b""
    while len(h) < 10: h += u.recv(10 - len(h))
    n = int.from_bytes(h[2:6], "big")
    while len(h) < n: h += u.recv(n - len(h))
    return h

srv = socket.socket(socket.AF_UNIX)
srv.bind(listen)
srv.listen(4)
while True:
    c, _ = srv.accept()
    u = socket.socket(socket.AF_UNIX)
    u.connect(upstream)
    while True:
        cmd = read_cmd(c)
        if cmd is None: break
        if retry != 0:
            retry -= 1 if retry > 0 else 0
            rep = RETRY
        else:
            u.sendall(cmd)
            rep = read_reply(u)
        if split:
            for i in range(0, len(rep), split):
                c.sendall(rep[i:i + split]); time.sleep(0.005)
        else:
            c.sendall(rep)
    c.close(); u.close()
