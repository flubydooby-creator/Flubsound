#!/usr/bin/env bash
# Flubsound - stage the desktop app and the plug-in from a CMake build into a
# test package (the CI `app` job uploads it as the "Flubsound-<os>" artifact).
#
#   tools/scripts/package-desktop.sh <build-dir> <out-dir> <os-label>
#
# Windows: a folder (the artifact zip keeps it as is).
# macOS:   one .zip made with ditto, so the .app bundles keep their symlinks
#          and executable bits (the artifact zip would drop both).
# Linux:   one .tar.gz, for the same reason.
# The builds are unsigned test builds (see TESTING.txt in the package).
set -euo pipefail

build=${1:?build dir}
out=${2:?output dir}
label=${3:?os label}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

stage="$out/Flubsound-$label"
rm -rf "$stage"
mkdir -p "$stage/Plug-ins"

app_dir="$build/app/FlubsoundPro_artefacts/Release"
fx_dir="$build/plugin/FlubsoundFX_artefacts/Release"

case "$label" in
    windows)
        cp "$app_dir/Flubsound Pro.exe" "$stage/"
        ;;
    macos)
        cp -R "$app_dir/Flubsound Pro.app" "$stage/"
        ;;
    linux)
        cp "$app_dir/Flubsound Pro" "$stage/"
        ;;
    *)
        echo "unknown os label: $label" >&2
        exit 2
        ;;
esac

# Plug-in formats that were built (VST3 and Standalone everywhere, AU on macOS).
for format in VST3 AU Standalone; do
    if [ -d "$fx_dir/$format" ]; then
        cp -R "$fx_dir/$format" "$stage/Plug-ins/"
    fi
done

cp "$root/tools/scripts/TESTING.txt" "$stage/TESTING.txt"
cp "$root/LICENSE" "$stage/" 2>/dev/null || true
git -C "$root" rev-parse --short HEAD > "$stage/VERSION.txt" 2>/dev/null || echo "unknown" > "$stage/VERSION.txt"

case "$label" in
    macos)
        (cd "$out" && ditto -c -k --sequesterRsrc --keepParent "Flubsound-$label" "Flubsound-$label.zip")
        rm -rf "$stage"
        ;;
    linux)
        tar -C "$out" -czf "$out/Flubsound-$label.tar.gz" "Flubsound-$label"
        rm -rf "$stage"
        ;;
esac

ls -la "$out"
