# Third-party components and licences

This project is MIT licensed (see `LICENSE`). The components below are used but not
vendored into this repository, except where noted.

| Component | Licence | How it is used |
|---|---|---|
| **uClibc** | LGPL-2.1 | statically linked into the prebuilt ARM binaries in `bin/` and `layer/sbin/`. Binaries are provided as a convenience; the full corresponding source of our code is in `src/`, and the uClibc build recipe is documented there. |
| **squashfs-tools** (`mksquashfs`, `unsquashfs`) | GPL-2.0 | host-side build tool, called as a subprocess. Not redistributed. |
| **busybox** | GPL-2.0 | already present on the camera (vendor firmware); we call its `telnetd`, `awk`, `sed`. Not redistributed. |
| **hostapd** 2.11 | BSD-3-Clause | optional AP provisioning. A prebuilt ARMv5/uClibc binary is redistributed in `ap-mode/out/hostapd` for convenience; `ap-mode/build.sh` rebuilds it from the untouched upstream release (`w1.fi`), and the licence text ships with that release. |
| **libnl 3.11** | LGPL-2.1 | linked *statically* into `ap-mode/out/hostapd` (nl80211 driver support). Rebuildable with `ap-mode/build.sh`; upstream at `github.com/thom311/libnl`. |
| **Frigate** | MIT | not part of this project: it is a client of the camera. Referenced in the docs only. |
| **Python standard library** | PSF-2.0 | all installer scripts use stdlib only. |

## Vendor firmware

The vendor program (`anyka_ipc`) **is not distributed here**. The installer dumps it
from your own camera and injects only our layer. The patchers in `patchers/` modify a
dump of your own camera, so no vendor code is redistributed either.

Our tools interoperate with that firmware through reverse engineering for
interoperability purposes, a practice generally recognised in most jurisdictions.

No affiliation with LSC, Tuya or Anyka.
