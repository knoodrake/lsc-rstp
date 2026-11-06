#!/usr/bin/env python3
"""Scan this repository for personal data before publishing it.

Looks for the things that leak a private LAN and a home setup: real IP ranges,
WiFi names and passphrases, MAC addresses, personal paths, tokens, MQTT
credentials and e-mail addresses. Exit code 1 if anything is found.

Usage:
    python3 tests/publish_check.py            # scan the repository
    python3 tests/publish_check.py --list     # also list the patterns
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PATTERNS = [
    ("wifi name",        re.compile(r"Livebox|SFR_|Freebox|Orange-|Bbox-|_2\.4G|_5G\b")),
    ("wifi passphrase",  re.compile(r"^\s*passphrase\s*=\s*\S{8,}", re.M)),
    # 192.168.1.50 / .1.1 are the documentation examples: only flag other ranges
    ("private LAN IP",   re.compile(r"\b192\.168\.(?!1\.(?:50|1)\b)\d{1,3}\.\d{1,3}\b")),
    ("MAC address",      re.compile(r"\b([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b")),
    ("home path",        re.compile(r"/home/[a-z][a-z0-9_-]+/")),
    ("long token",       re.compile(r"\b[0-9a-f]{40,}\b|\beyJ[A-Za-z0-9._-]{20,}")),
    ("mqtt credentials", re.compile(r"mqtt://\S+:\S+@|frigate.{0,10}password|password.{0,10}frigate", re.I)),
    ("e-mail",           re.compile(r"\b[\w.+-]+@[\w-]+\.[\w.]{2,}\b")),
    ("hostname",         re.compile(r"\bmainframe\d*\b|\bknoodrake\b", re.I)),
]

SKIP_DIRS = {".git", "__pycache__", "build", "dumps"}
SKIP_FILES = {"publish_check.py", "MD5SUMS"}
TEXT_EXT = {".md", ".py", ".sh", ".c", ".h", ".s", ".S", ".txt", ".yaml", ".yml",
            ".ini", ".tmpl", ".json", ".cfg"}


def _tracked():
    """Files git tracks (falls back to a plain walk outside a checkout)."""
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True,
                             text=True, check=True).stdout
        return [os.path.join(ROOT, p) for p in out.splitlines() if p]
    except Exception:
        return None


def walk():
    tracked = _tracked()
    if tracked is not None:
        for path in tracked:
            if os.path.basename(path) in SKIP_FILES:
                continue
            if os.path.splitext(path)[1].lower() in TEXT_EXT:
                yield path
        return
    for d, dirs, files in os.walk(ROOT):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in files:
            if f in SKIP_FILES:
                continue
            if os.path.splitext(f)[1].lower() in TEXT_EXT:
                yield os.path.join(d, f)


def main():
    listing = "--list" in sys.argv
    hits = []
    scanned = 0
    for path in walk():
        scanned += 1
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except Exception:
            continue
        rel = os.path.relpath(path, ROOT)
        for name, pat in PATTERNS:
            # publish.sh legitimately names the target repository
            if name in ("e-mail", "hostname") and rel == "publish.sh":
                continue
            for n, line in enumerate(text.splitlines(), 1):
                line = re.sub(r"\{\{[^}]*\}\}", " ", line)   # {{PLACEHOLDER}} = safe
                if pat.search(line):
                    hits.append((rel, n, name, line.strip()[:110]))
    print("scanned %d text files" % scanned)
    if not hits:
        print("no personal data pattern found — OK to publish")
        return 0
    print("%d potential leak(s):\n" % len(hits))
    for f, n, name, line in hits:
        print("  %-34s %4d  [%s] %s" % (f, n, name, line))
    if listing:
        print("\npatterns: %s" % ", ".join(p for p, _ in PATTERNS))
    return 1


if __name__ == "__main__":
    sys.exit(main())
