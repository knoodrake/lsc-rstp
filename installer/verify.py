#!/usr/bin/env python3
"""Post-install checks for an LSC PTZ camera.

Answers one question: is this camera ready for Frigate (RTSP + ONVIF PTZ +
presets), and what exactly is missing if not. Nothing is calibrated here: only
the standby position (`level`) is a physical installation choice, and you can
record it with --level x,y.

Usage:
    verify.py <CAM_IP>                  # checks only
    verify.py <CAM_IP> --level 0.30,0.00  # aim here, then save the `level` preset
"""
import argparse
import base64
import socket
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

NS = {
    "s": "http://www.w3.org/2003/05/soap-envelope",
    "tptz": "http://www.onvif.org/ver20/ptz/wsdl",
}
TOL = 0.02
MAX_STEPS = 24

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %-52s %s" % ("OK  " if ok else "FAIL", name, detail))
    return ok


def soap(ip, port, action, body):
    env = ('<?xml version="1.0" encoding="UTF-8"?>'
           '<s:Envelope xmlns:s="%s" xmlns:tptz="%s"><s:Body>%s</s:Body></s:Envelope>'
           % (NS["s"], NS["tptz"], body))
    req = urllib.request.Request(
        "http://%s:%d/onvif/ptz_service" % (ip, port), data=env.encode(),
        headers={"Content-Type": 'application/soap+xml; charset=utf-8',
                 "Authorization": "Basic " + base64.b64encode(b"admin:admin").decode()})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")
    except Exception as e:
        return 0, str(e)


def status(ip, port):
    st, txt = soap(ip, port, "GetStatus",
                   '<tptz:GetStatus><tptz:ProfileToken>Profile_1</tptz:ProfileToken>'
                   '</tptz:GetStatus>')
    if st != 200:
        return None
    try:
        root = ET.fromstring(txt)
        node = root.find(".//{http://www.onvif.org/ver10/schema}PanTilt")
        return float(node.get("x")), float(node.get("y"))
    except Exception:
        return None


def relative_move(ip, port, dx, dy):
    body = ('<tptz:RelativeMove><tptz:ProfileToken>Profile_1</tptz:ProfileToken>'
            '<tptz:Translation><tt:PanTilt x="%.4f" y="%.4f" '
            'space="http://www.onvif.org/ver10/tptz/PanTiltSpaces/'
            'TranslationSpaceFov" xmlns:tt="http://www.onvif.org/ver10/schema"/>'
            '</tptz:Translation></tptz:RelativeMove>' % (dx, dy))
    return soap(ip, port, "RelativeMove", body)[0] == 200


def set_preset(ip, port, name):
    st, _ = soap(ip, port, "SetPreset",
                 '<tptz:SetPreset><tptz:ProfileToken>Profile_1</tptz:ProfileToken>'
                 '<tptz:PresetName>%s</tptz:PresetName></tptz:SetPreset>' % name)
    return st == 200


def presets(ip, port):
    st, txt = soap(ip, port, "GetPresets",
                   '<tptz:GetPresets><tptz:ProfileToken>Profile_1</tptz:ProfileToken>'
                   '</tptz:GetPresets>')
    if st != 200:
        return []
    # preset tokens are emitted as "preset_<name>"
    names = []
    for chunk in txt.split("preset_")[1:]:
        name = chunk.split("<")[0].strip()
        if name and name not in names:
            names.append(name)
    return names[:16]


def tcp(ip, port, timeout=3):
    try:
        s = socket.create_connection((ip, port), timeout=timeout)
        s.close()
        return True
    except Exception:
        return False


def rtsp(ip, path="/main_ch"):
    try:
        s = socket.create_connection((ip, 554), timeout=5)
        s.sendall(("DESCRIBE rtsp://%s:554%s RTSP/1.0\r\nCSeq: 1\r\n"
                   "Accept: application/sdp\r\nUser-Agent: lsc-rstp\r\n\r\n"
                   % (ip, path)).encode())
        s.settimeout(6)
        data = s.recv(2048).decode(errors="replace")
        s.close()
        return data.startswith("RTSP/1.0 200")
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description="LSC PTZ post-install checks")
    ap.add_argument("cam")
    ap.add_argument("--onvif-port", type=int, default=8080)
    ap.add_argument("--level", help="record the `level` preset at x,y (FOV space)")
    a = ap.parse_args()
    ip, port = a.cam, a.onvif_port

    print("=== %s (ONVIF :%d) ===" % (ip, port))

    check("RTSP :554 listening", tcp(ip, 554))
    check("RTSP /main_ch DESCRIBE 200", rtsp(ip, "/main_ch"))
    check("ONVIF :%d listening" % port, tcp(ip, port))

    hz = ""
    try:
        with urllib.request.urlopen("http://%s:%d/healthz" % (ip, port), timeout=10) as r:
            hz = r.read().decode(errors="replace")
        check("healthz HTTP 200", r.status == 200, hz[:80])
    except Exception as e:
        check("healthz HTTP 200", False, str(e))
    check("PTZ backend answering (ptzd=PONG)", '"ptzd": "PONG"' in hz or 'PONG' in hz)
    check("ONVIF served by the camera itself", 'on-camera' in hz)

    st, _ = soap(ip, port, "GetServiceCapabilities",
                 '<tptz:GetServiceCapabilities/>')
    check("GetServiceCapabilities 200", st == 200, "HTTP %s" % st)

    import subprocess, os
    here = os.path.dirname(os.path.abspath(__file__))
    val = os.path.join(os.path.dirname(here), "tests", "validate_cam.py")
    if os.path.exists(val):
        out = subprocess.run([sys.executable, val, ip, str(port)],
                             capture_output=True, text=True).stdout
        last = [l for l in out.splitlines() if "VERDICT" in l or "OK /" in l]
        check("ONVIF conformance suite", "VERDICT: OK" in out,
              (last[-1] if last else out.strip().splitlines()[-1][:70]))
    else:
        check("ONVIF conformance suite", False, "tests/validate_cam.py not found")

    pr = presets(ip, port)
    check("preset 'home' present", any("home" in p for p in pr), str(pr)[:70])

    if a.level:
        tx, ty = [float(v) for v in a.level.split(",")]
        print("   aiming at (%+.3f, %+.3f) then saving 'level'..." % (tx, ty))
        ok = False
        for i in range(MAX_STEPS):
            pos = status(ip, port)
            if pos is None:
                break
            dx, dy = tx - pos[0], ty - pos[1]
            if abs(dx) < TOL and abs(dy) < TOL:
                ok = True
                print("   reached in %d step(s): (%+.3f, %+.3f)" % (i, pos[0], pos[1]))
                break
            if not relative_move(ip, port, max(-0.5, min(0.5, dx)),
                                 max(-0.5, min(0.5, dy))):
                break
            time.sleep(0.6)
        check("moved to the requested position", ok)
        check("preset 'level' saved", set_preset(ip, port, "level") if ok else False)

    check("preset 'level' present", any("level" in p for p in presets(ip, port)))

    ok = sum(1 for _, o, _ in results if o)
    bad = [n for n, o, _ in results if not o]
    print("\nVERDICT: %s (%d OK / %d KO)"
          % ("READY" if not bad else "NEEDS WORK", ok, len(bad)))
    if bad:
        print("missing: " + ", ".join(bad))
    else:
        print("""
Frigate snippet:

cameras:
  my_lsc_cam:
    ffmpeg:
      inputs:
        - path: rtsp://%s:554/main_ch
          roles: [detect, record]
    detect:
      width: 640
      height: 360
    onvif:
      host: %s
      port: %d
      user: admin
      password: admin
      autotracking:
        enabled: true
        return_preset: level
""" % (ip, ip, port))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
