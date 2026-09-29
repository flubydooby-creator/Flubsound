#!/usr/bin/env bash
# Flubsound Pro - macOS disk image (docs/11 E54), made by
# tools/scripts/package-desktop.sh in the CI `app` job (macos-14):
#
#   installer/macos/make-dmg.sh <staged folder> <out.dmg> [volume name]
#
# The image holds the staged folder's contents (Flubsound Pro.app,
# Plug-ins/, TESTING.txt, ...) plus an "Applications" link to drag the app
# onto. Compressed (UDZO), unsigned and not notarised: Gatekeeper asks on the
# first start (see TESTING.txt). hdiutil on CI runners sometimes fails with
# "Resource busy"; it is retried a few times.
set -euo pipefail

src=${1:?staged folder}
out=${2:?output .dmg}
volume=${3:-Flubsound Pro}

[ -d "$src" ] || { echo "make-dmg: $src is not a folder" >&2; exit 2; }
command -v hdiutil > /dev/null || { echo "make-dmg: hdiutil not found (macOS only)" >&2; exit 2; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# ditto keeps the bundles' symlinks, permissions and extended attributes.
ditto "$src" "$work/$volume"
ln -s /Applications "$work/$volume/Applications"

rm -f "$out"
for attempt in 1 2 3 4 5; do
    if hdiutil create -volname "$volume" -srcfolder "$work/$volume" -fs HFS+ -format UDZO -ov "$out"; then
        hdiutil verify "$out"
        exit 0
    fi
    echo "make-dmg: hdiutil create failed (attempt $attempt), retrying" >&2
    sleep $((attempt * 3))
done
exit 1
