#!/usr/bin/env python3
"""Camera telnet helper: run commands on cam_3 root shell (port 24)."""
import os, socket, sys, time

def run(cmds, port=24, host="192.168.1.50", wait=1.5, endwait=1.0):
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(2)

    def rd(t):
        b = bytearray()
        t0 = time.time()
        while time.time() - t0 < t:
            try:
                d = s.recv(8192)
                if not d:
                    break
                b += d
            except Exception:
                break
        return bytes(b).decode(errors="ignore")

    out = []
    rd(1)
    for c in cmds:
        s.sendall((c + "\r\n").encode())
        time.sleep(0.3)
        out.append(("$ " + c, rd(wait if len(c) < 200 else endwait)))
    s.close()
    return out

if __name__ == "__main__":
    cmds = sys.argv[1:]
    host = os.environ.get("CAM_HOST", "192.168.1.50")  # multi-cam: cam_2 = 192.168.1.50
    port = int(os.environ.get("CAM_PORT", "24"))       # cam_3 card-free = 24, Tuya = 23
    for label, out in run(cmds, host=host, port=port):
        print(label)
        print(out)
        print("~" * 40)
