#!/usr/bin/env bash
# Push this repository to GitHub.
#
#   ./publish.sh                      # pushes to git@github.com:knoodrake/lsc-rstp.git
#   OWNER=me NAME=my-cam ./publish.sh
#
# The remote repository must exist (GitHub does not create repositories from a
# push). With the GitHub CLI installed:
#
#   gh repo create <OWNER>/<NAME> --public --source . --remote origin --push
#
# Without it, create the repository in the web UI, then run this script.
set -euo pipefail
cd "$(dirname "$0")"

OWNER="${OWNER:-knoodrake}"
NAME="${NAME:-lsc-rstp}"
REMOTE="${REMOTE:-git@github.com:$OWNER/$NAME.git}"

if ! git rev-parse --git-dir > /dev/null 2>&1; then
    echo "not a git repository" >&2
    exit 1
fi

echo "remote : $REMOTE"
git remote remove origin 2>/dev/null || true
git remote add origin "$REMOTE"

echo "pushing $(git rev-parse --short HEAD) ($(git rev-list --count HEAD) commit(s))..."
git push -u origin main

echo
echo "done — https://github.com/$OWNER/$NAME"
echo "do not forget the repository metadata:"
echo "  description : Local RTSP + ONVIF + PTZ for LSC/Tuya Anyka cameras, no cloud"
echo "  topics      : lsc tuya anyka rtsp onvif ptz firmware camera frigate home-assistant"
