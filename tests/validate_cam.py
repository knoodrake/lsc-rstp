#!/usr/bin/env python3
"""
Validation de conformité du démon ONVIF embarqué (lsc_onvif) sur une caméra LSC.

Rejoue, dans l'ordre, la séquence exacte que Frigate exécute dans
`frigate/ptz/onvif.py` (update_xaddrs -> GetProfiles -> GetVideoSources ->
GetConfigurationOptions -> GetServiceCapabilities -> GetStatus) puis vérifie
les mouvements PTZ (ContinuousMove / Stop / RelativeMove en boucle fermée).

Usage :
    python3 validate_cam.py [IP] [PORT]
défaut : 192.168.1.50 8080

Sortie : une ligne OK/KO par critère + VERDICT ; code retour 0 si 0 KO.
Aucun paquet RTSP, aucun accès au shim hôte : uniquement le port ONVIF de la caméra.
"""
import sys
import urllib.request
import urllib.error
import xml.etree.ElementTree as ET
import time

NS = {
    "s": "http://www.w3.org/2003/05/soap-envelope",
    "s11": "http://schemas.xmlsoap.org/soap/envelope/",
    "tt": "http://www.onvif.org/ver10/schema",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tptz": "http://www.onvif.org/ver20/ptz/wsdl",
    "wsa": "http://www.w3.org/2005/08/addressing",
}
ENV12 = NS["s"]
ENV11 = NS["s11"]

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("OK  " if ok else "KO  ") + name + (("  [" + detail + "]") if detail else ""))
    return ok


def soap_envelope(action, body, ns_hint):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<s:Envelope xmlns:s="%s" xmlns:tt="%s" xmlns:tds="%s" xmlns:trt="%s" '
        'xmlns:tptz="%s" xmlns:wsa="%s">'
        '<s:Header><wsa:Action>%s</wsa:Action>'
        '<wsa:MessageID>urn:uuid:00000000-0000-4000-8000-%012d</wsa:MessageID></s:Header>'
        '<s:Body>%s</s:Body></s:Envelope>'
        % (ENV12, NS["tt"], NS["tds"], NS["trt"], NS["tptz"], NS["wsa"],
           action, int(time.time() * 1000) % 10**12, body)
    )


def post(ip, port, path, xml, timeout=8):
    url = "http://%s:%d/onvif/%s" % (ip, port, path)
    req = urllib.request.Request(
        url, data=xml.encode(), headers={
            "Content-Type": 'application/soap+xml; charset=utf-8; action="x"',
            "Connection": "close",
        }, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        return e.code, body
    except Exception as e:  # noqa: BLE001
        return 0, "%s: %s" % (type(e).__name__, e)


def parse(xml):
    try:
        return ET.fromstring(xml), None
    except Exception as e:  # noqa: BLE001
        return None, str(e)


def get(path, action, body, ip, port):
    st, txt = post(ip, port, path, soap_envelope(action, body, None))
    return st, txt


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print("usage: validate_cam.py <CAM_IP> [ONVIF_PORT]")
        print("       runs the in-camera ONVIF PTZ conformance suite (22-23 checks)")
        return 0
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.50"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    print("=== cible %s:%d (démon ONVIF embarqué)" % (ip, port))

    # ---- 1. GetCapabilities (brut, sans auth) : Frigate étape 1 de update_xaddrs
    st, txt = get("device_service", "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                  '<tds:GetCapabilities xmlns:tds="%s"/>' % NS["tds"], ip, port)
    root, err = parse(txt)
    if not check("GetCapabilities: HTTP 200 + XML", st == 200 and root is not None,
                 "status=%d %s" % (st, err or "")):
        return summary()
    xaddr = {}
    for svc_name, path in (("Device", "device_service"), ("Media", "media_service"),
                           ("PTZ", "ptz_service"), ("Events", "events_service")):
        # tds:Capabilities porte les enfants directs (Device/Events/Media/PTZ) ;
        # l'ancien wrapper tt:Services n'est plus emis (mais tolere si present).
        el = root.find(".//tt:Capabilities/tt:%s/tt:XAddr" % svc_name, NS)
        if el is None:
            el = root.find(".//tt:Capabilities/tt:Services/tt:%s/tt:XAddr" % svc_name, NS)
        text = (el.text or "").strip() if el is not None else ""
        xaddr[svc_name] = text
        check("Capabilities>%s>XAddr" % svc_name, text.startswith("http://"), text or "(absent)")

    # ---- 2. GetProfiles : VideoEncoderConfiguration + PTZConfiguration exploitable
    st, txt = get("media_service", "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                  '<trt:GetProfiles xmlns:trt="%s"/>' % NS["trt"], ip, port)
    root, err = parse(txt)
    if not check("GetProfiles: HTTP 200 + XML", st == 200 and root is not None,
                 "status=%d %s" % (st, err or "")):
        return summary()
    profiles = root.findall(".//trt:Profiles", NS)
    check("GetProfiles: >=1 profil", len(profiles) >= 1, "%d profil(s)" % len(profiles))
    usable = []
    for p in profiles:
        enc = p.find("tt:VideoEncoderConfiguration", NS)
        ctl = p.find("tt:PTZConfiguration", NS)
        vel = ctl.find("tt:DefaultContinuousPanTiltVelocitySpace", NS) if ctl is not None else None
        ok = enc is not None and ctl is not None and vel is not None and (vel.text or "").strip()
        if ok:
            w = enc.find("tt:Resolution/tt:Width", NS)
            h = enc.find("tt:Resolution/tt:Height", NS)
            usable.append((p.get("token"), (w.text if w is not None else "?"),
                           (h.text if h is not None else "?")))
        check("profil %s: encoder+PTZ+DefaultContinuousPanTiltVelocitySpace" % p.get("token"),
              ok, "vel=%s" % ((vel.text or "") if vel is not None else "(absent)"))
    check("GetProfiles: au moins 1 profil PTZ exploitable par Frigate", usable,
          str(usable))

    # ---- 3. GetStreamUri (Frigate ne l'appelle pas au PTZ init mais c'est utile)
    if usable:
        tok = usable[0][0]
        st, txt = get("media_service", "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
                      '<trt:GetStreamUri xmlns:trt="%s"><trt:ProfileToken>%s</trt:ProfileToken>'
                      '<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>'
                      '<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>'
                      '</trt:StreamSetup></trt:GetStreamUri>' % (NS["trt"], tok), ip, port)
        root, err = parse(txt)
        uri = ""
        if root is not None:
            el = root.find(".//tt:MediaUri/tt:Uri", NS)
            uri = (el.text or "").strip() if el is not None else ""
        check("GetStreamUri non vide", uri.startswith("rtsp://"), uri or "(vide)")

    # ---- 4. GetVideoSources
    st, txt = get("media_service", "http://www.onvif.org/ver10/media/wsdl/GetVideoSources",
                  '<trt:GetVideoSources xmlns:trt="%s"/>' % NS["trt"], ip, port)
    root, err = parse(txt)
    srcs = root.findall(".//tt:VideoSources", NS) if root is not None else []
    check("GetVideoSources: >=1 source", len(srcs) >= 1,
          "%d source(s) status=%d" % (len(srcs), st))

    # ---- 5. GetConfigurationOptions : espace TranslationSpaceFov (=> Frigate pt-r-fov)
    tok = usable[0][0] if usable else "Profile_1"
    st, txt = get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/GetConfigurationOptions",
                  '<tptz:GetConfigurationOptions xmlns:tptz="%s">'
                  '<tptz:ConfigurationToken>PTZConfig_1</tptz:ConfigurationToken>'
                  '</tptz:GetConfigurationOptions>' % NS["tptz"], ip, port)
    root, err = parse(txt)
    fov = False
    uris = []
    if root is not None:
        for sp in root.findall(".//tt:RelativePanTiltTranslationSpace", NS):
            u = sp.find("tt:URI", NS)
            if u is not None and u.text:
                uris.append(u.text.strip())
        fov = any("TranslationSpaceFov" in u for u in uris)
    check("GetConfigurationOptions: RelativePanTiltTranslationSpace x%d dont FOV" % len(uris),
          fov, "; ".join(uris[-2:]))

    # ---- 6. GetServiceCapabilities : attribut MoveStatus (autotracking Frigate)
    st, txt = get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/GetServiceCapabilities",
                  '<tptz:GetServiceCapabilities xmlns:tptz="%s"/>' % NS["tptz"], ip, port)
    root, err = parse(txt)
    caps = root.find(".//tptz:Capabilities", NS) if root is not None else None
    mv = caps.get("MoveStatus") if caps is not None else None
    check("GetServiceCapabilities: MoveStatus=true (attribut)", mv == "true", str(caps.attrib) if caps is not None else "(absent)")

    # ---- 7. GetStatus : MoveStatus/PanTilt lisible
    def get_status():
        st, txt = get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/GetStatus",
                      '<tptz:GetStatus xmlns:tptz="%s"><tptz:ProfileToken>%s</tptz:ProfileToken>'
                      '</tptz:GetStatus>' % (NS["tptz"], tok), ip, port)
        root, err = parse(txt)
        if root is None:
            return None, None, None
        pos = root.find(".//tptz:GetStatusResponse/tt:PTZStatus/tt:Position/tt:PanTilt", NS)
        mv = root.find(".//tptz:GetStatusResponse/tt:PTZStatus/tt:MoveStatus/tt:PanTilt", NS)
        x = float(pos.get("x")) if pos is not None else None
        y = float(pos.get("y")) if pos is not None else None
        return x, y, (mv.text if mv is not None else None)

    x0, y0, ms0 = get_status()
    check("GetStatus: position + MoveStatus.PanTilt", x0 is not None and ms0 is not None,
          "x=%s y=%s move=%s" % (x0, y0, ms0))

    # ---- 8. ContinuousMove -> doit passer MOVING -> Stop -> IDLE
    if x0 is not None:
        st, txt = get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove",
                      '<tptz:ContinuousMove xmlns:tptz="%s"><tptz:ProfileToken>%s</tptz:ProfileToken>'
                      '<tptz:Velocity><tt:PanTilt x="0.35" y="0"/></tptz:Velocity>'
                      '</tptz:ContinuousMove>' % (NS["tptz"], tok), ip, port)
        check("ContinuousMove: HTTP 200", st == 200, "status=%d" % st)
        time.sleep(0.9)
        _, _, ms_moving = get_status()
        check("GetStatus pendant le mouvement = MOVING", ms_moving == "MOVING", str(ms_moving))
        time.sleep(0.5)
        st, _ = get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/Stop",
                    '<tptz:Stop xmlns:tptz="%s"><tptz:ProfileToken>%s</tptz:ProfileToken>'
                    '<tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>false</tptz:Zoom></tptz:Stop>'
                    % (NS["tptz"], tok), ip, port)
        check("Stop: HTTP 200", st == 200, "status=%d" % st)
        time.sleep(1.2)
        x1, y1, ms1 = get_status()
        check("GetStatus après Stop = IDLE", ms1 == "IDLE", str(ms1))
        check("ContinuousMove a bien bougé (|dx|>0.02)", x1 is not None and abs(x1 - x0) > 0.02,
              "dx=%.4f" % ((x1 - x0) if x1 is not None else 0))

        # ---- 8bis. recentrage : la caméra peut être en butée (x=±1.0) après le
        # ContinuousMove, et un RelativeMove de +0.10 sature alors à 0 (dx mesuré 0).
        xa, _, _ = get_status()
        if xa is not None and abs(xa - 0.5) > 0.15:
            get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/RelativeMove",
                '<tptz:RelativeMove xmlns:tptz="%s"><tptz:ProfileToken>%s</tptz:ProfileToken>'
                '<tptz:Translation><tt:PanTilt x="%.3f" y="0"/></tptz:Translation>'
                '</tptz:RelativeMove>' % (NS["tptz"], tok, 0.5 - xa), ip, port)
            time.sleep(3.5)
            check("recentrage pan avant RelativeMove (headroom)",
                  abs((get_status()[0] or 0.0) - 0.5) <= 0.2,
                  "x=%.4f" % (get_status()[0] or 0.0))

        # ---- 9. RelativeMove en boucle fermée, espace générique puis FOV
        for space_label, dx, tol in (("générique", 0.10, 0.06), ("FOV", 0.10, 0.06)):
            xa, _, _ = get_status()
            st, txt = get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/RelativeMove",
                          '<tptz:RelativeMove xmlns:tptz="%s"><tptz:ProfileToken>%s</tptz:ProfileToken>'
                          '<tptz:Translation><tt:PanTilt x="%.3f" y="0"/></tptz:Translation>'
                          '</tptz:RelativeMove>' % (NS["tptz"], tok, dx), ip, port)
            ok_st = st == 200
            time.sleep(2.2)
            xb, _, _ = get_status()
            moved = (xb - xa) if (xa is not None and xb is not None) else 0.0
            check("RelativeMove(%s) closed-loop dx=%.3f" % (space_label, dx),
                  ok_st and abs(moved - dx) <= tol,
                  "mesuré dx=%.4f (attendu %.3f ±%.3f)" % (moved, dx, tol))
            # retour au centre
            get("ptz_service", "http://www.onvif.org/ver20/ptz/wsdl/RelativeMove",
                '<tptz:RelativeMove xmlns:tptz="%s"><tptz:ProfileToken>%s</tptz:ProfileToken>'
                '<tptz:Translation><tt:PanTilt x="%.3f" y="0"/></tptz:Translation>'
                '</tptz:RelativeMove>' % (NS["tptz"], tok, -moved), ip, port)
            time.sleep(2.0)

    return summary()


def summary():
    fails = [r for r in results if not r[1]]
    print("=== VERDICT: %s (%d OK / %d KO)" %
          ("OK" if not fails else "KO", len(results) - len(fails), len(fails)))
    for name, _, det in fails:
        print("    KO: %s  %s" % (name, det))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
