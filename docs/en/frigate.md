# Frigate

Frigate talks to the camera in two independent ways, and both are local:

* **video** over RTSP,
* **PTZ** over ONVIF (`:8080`, user `admin`, password `admin`).

```yaml
cameras:
  lsc_cam:
    ffmpeg:
      inputs:
        - path: rtsp://192.168.1.50:554/main_ch
          roles: [detect, record]
        # optional, if your build exposes the sub-stream:
        # - path: rtsp://192.168.1.50:554/videoSub
        #   roles: [detect]
    detect:
      width: 640
      height: 360
    record:
      enabled: true
    onvif:
      host: 192.168.1.50
      port: 8080
      user: admin
      password: admin
      autotracking:
        enabled: true
        return_preset: level
        timeout: 10
```

## Autotracking

Two things matter for autotracking on this hardware:

1. **`return_preset` must be `level`, never `home`.**
   `home` is the folded privacy position (the head points at the ceiling), so the
   camera would come back to a useless view.
2. **The `level` preset must exist.**
   Save it once with `installer/verify.py <ip> --level x,y`. It is written to
   `/etc/config/lsc_presets.txt` and restored at every boot, and the camera also
   *moves back to `level` by itself at startup* — so a camera that used to stare
   at the ceiling after a power cycle now comes back to its watch position.

With those two rules, and the tilt calibration described in
[architecture](architecture.md), autotracking works: the camera tracks a moving person
and returns to `level` after the timeout.

## Detection on the main stream

On builds without `/videoSub`, detection runs on the 1080p stream (Frigate downsamples
to its detect resolution). It works, but it is heavier and slightly more fragile: you
may see occasional ffmpeg restarts in the Frigate log. That is a build limitation, not
a configuration error.

## PTZ from Home Assistant

PTZ actions are exposed through Frigate: publish to
`frigate/<camera>/ptz` with `{"action": "move_left"}` style payloads, or call the
Frigate PTZ service. The camera itself also accepts plain ONVIF PTZ, so any ONVIF
client works.
