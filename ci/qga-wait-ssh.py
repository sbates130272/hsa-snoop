#!/usr/bin/env python3
"""
QGA SSH readiness check: exits 0 when ssh.service is active in the guest.

Usage: python3 qga-wait-ssh.py <qga-socket-path>
"""
import socket
import json
import sys
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(sys.argv[1])
sock.settimeout(5)


def send_recv(obj):
    sock.send((json.dumps(obj) + "\n").encode())
    buf = b""
    for _ in range(20):
        try:
            buf += sock.recv(4096)
            return json.loads(buf.decode())
        except (json.JSONDecodeError, socket.timeout):
            pass
    return None


send_recv({"execute": "guest-sync", "arguments": {"id": 99}})
r = send_recv(
    {
        "execute": "guest-exec",
        "arguments": {
            "path": "/bin/systemctl",
            "arg": ["is-active", "ssh.service"],
            "capture-output": True,
        },
    }
)
if not r:
    sys.exit(1)
time.sleep(1)
r2 = send_recv({"execute": "guest-exec-status", "arguments": {"pid": r["return"]["pid"]}})
sock.close()
sys.exit(0 if r2 and r2["return"].get("exitcode") == 0 else 1)
