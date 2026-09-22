#!/usr/bin/env python3
"""
QGA bootstrap: assign TAP bridge IP and start sshd in the guest.

Usage: python3 qga-bootstrap.py <qga-socket-path>

Connects to the QEMU Guest Agent socket, waits for multi-user.target,
then assigns 172.16.223.100/24 (gw 172.16.223.1) to enp0s3 and starts
sshd.  Run via `docker exec` inside the qemu container.
"""
import socket
import json
import sys
import time

qga_path = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(qga_path)
sock.settimeout(5)


def send_recv(obj):
    sock.send((json.dumps(obj) + "\n").encode())
    buf = b""
    for _ in range(40):
        try:
            buf += sock.recv(4096)
            return json.loads(buf.decode())
        except (json.JSONDecodeError, socket.timeout):
            pass
    return None


send_recv({"execute": "guest-sync", "arguments": {"id": 1}})

for _ in range(60):
    resp = send_recv(
        {
            "execute": "guest-exec",
            "arguments": {
                "path": "/bin/systemctl",
                "arg": ["is-active", "multi-user.target"],
                "capture-output": True,
            },
        }
    )
    if resp and "return" in resp:
        time.sleep(1)
        r2 = send_recv(
            {
                "execute": "guest-exec-status",
                "arguments": {"pid": resp["return"]["pid"]},
            }
        )
        if r2 and r2["return"].get("exitcode") == 0:
            print("Guest reached multi-user.target")
            break
    time.sleep(2)

for label, cmd in [
    ("assign IP", "ip addr add 10.0.2.15/24 dev enp0s3 2>/dev/null || true"),
    ("add route", "ip route add default via 10.0.2.2 dev enp0s3 2>/dev/null || true"),
    ("start sshd", "systemctl start ssh.service && sleep 3"),
]:
    resp = send_recv(
        {
            "execute": "guest-exec",
            "arguments": {"path": "/bin/bash", "arg": ["-c", cmd], "capture-output": True},
        }
    )
    time.sleep(1)
    if resp and "return" in resp:
        r2 = send_recv(
            {
                "execute": "guest-exec-status",
                "arguments": {"pid": resp["return"]["pid"]},
            }
        )
        ec = r2["return"].get("exitcode") if r2 else "?"
        print(f"{label}: exit={ec}")

sock.close()
print("Guest network and sshd bootstrapped")
