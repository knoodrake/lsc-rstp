# Changelog

All dates are local (Europe/Berlin). This project ships **patchers**, never vendor
firmware images.

## Unreleased

### Fixed
- **installer: never reboot before the flash really finished.** `flash_app.py`
  looked for `FLASH_OK` in the output of a command whose *echo* contained the
  string, so it reported success ~3 s after launch while the mtd7 write takes
  ~52–60 s — an `install.py --reboot` at that moment cut `mtd_update` in half
  and bricked the camera. It now lists `/tmp/FLASH_*` (no literal marker in the
  command) and waits for the real file.
- **installer: transfer verification no longer depends on the shell ack.** On the
  flakiest camera the `nc` transfer completes but the trailing `echo` never comes
  back; `transfer()` now checks the md5 in a fresh telnet session.
- **onvif: presets survive reboots.** They live in RAM by default, so
  `return_preset: level` silently fell back to `home` (folded) after every power
  cycle — the camera stared at the ceiling. Presets are now written to
  `/etc/config/lsc_presets.txt` (atomic, jffs2) and restored at startup.
- **onvif: the camera drives itself back to `level` at startup**, so a power
  cycle no longer leaves it pointing wherever the motors stopped.
- **installer: prefer the LSC shell port** `:24` over the vendor login telnet on
  `:23`, which some builds expose.

### Added
- `tilt_sign=-1` in `/etc/config/lsc_onvif.conf`: per-camera tilt direction, for
  cameras whose tilt axis is mounted the other way round (up/down inverted in
  Frigate).
- `docs/en/recovery.md`: the firmware's own SD recovery (`update.tar`), which
  restores any partition from a FAT32 card when the APP does not mount.
- `publish.sh` and a GitHub Actions workflow (python syntax, CLI `--help`,
  layer checksums, PII scan, shell syntax).

## 0.1 — 2026-11-05
- First public tree: `installer/` (probe → dump → build → flash → verify),
  `layer/` (ONVIF daemon, `ptzd`, launcher, factory INI template), `docs/` EN+FR,
  `patchers/`, `sd/`, `examples/`.
