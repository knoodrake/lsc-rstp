#!/usr/bin/env python3
"""Run ONE command on a camera telnet (CAM_HOST env, default cam_3) and keep the session open until the
command prints a sentinel (or timeout), so long transfers survive.

Usage:
  python3 cam_hold.py '<cmd>' [sentinel] [timeout_s]

Prints everything received (already includes the echoed command). Exit 0 only
if the sentinel was seen before the timeout.
"""
import os, socket, sys, time

HOST = os.environ.get("CAM_HOST", "127.0.0.1")
PORT = int(os.environ.get("CAM_PORT", "24"))


def main() -> int:
    cmd = sys.argv[1]
    sentinel = sys.argv[2] if len(sys.argv) > 2 else "__DONE__"
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 120.0

    s = socket.create_connection((HOST, PORT), timeout=5)
    s.settimeout(1.0)
    buf = bytearray()

    def drain(t):
        t0 = time.time()
        while time.time() - t0 < t:
            try:
                d = s.recv(8192)
                if not d:
                    return
                buf.extend(d)
            except socket.timeout:
                continue
            except OSError:
                return

    drain(0.8)                      # consume banner
    s.sendall((cmd + "\r\n").encode())

    # Le terminal renvoie l'echo de la commande: si le sentinelle y figure
    # (cas `...; echo UP_DONE`), il faut attendre sa (n+1)ième occurrence.
    need = cmd.count(sentinel) + 1
    sb = sentinel.encode()
    seen = False
    t0 = time.time()
    while time.time() - t0 < timeout:
        drain(0.5)
        if bytes(buf).count(sb) >= need:
            seen = True
            break
    drain(0.4)
    s.close()
    sys.stdout.write(bytes(buf).decode(errors="ignore"))
    sys.stdout.write("\n")
    return 0 if seen else 3


if __name__ == "__main__":
    sys.exit(main())
