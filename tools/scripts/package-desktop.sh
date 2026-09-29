#!/usr/bin/env bash
# Flubsound - stage the desktop app and the plug-in from a CMake build into a
# test package (the CI `app` job uploads it as the "Flubsound-<os>" artifact).
#
#   tools/scripts/package-desktop.sh <build-dir> <out-dir> <os-label>
#
# Windows: a folder (the artifact zip keeps it as is) and, when Inno Setup's
#          ISCC.exe is found ($ISCC, PATH or its default folder), the
#          installer FlubsoundPro-Setup-<version>.exe next to it
#          (installer/windows/FlubsoundPro.iss). FLUB_REQUIRE_INSTALLER=1
#          makes a missing ISCC an error (CI).
# macOS:   one .zip made with ditto, so the .app bundles keep their symlinks
#          and executable bits (the artifact zip would drop both), and the
#          same contents as Flubsound-macos.dmg (installer/macos/make-dmg.sh;
#          a failed dmg is a warning, the .zip is still there).
# Linux:   one .tar.gz, for the same reason, with a menu entry
#          (flubsound-pro.desktop) and install.sh (installer/linux).
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
cp "$root/AUTHORS.md" "$stage/"
cp "$root/LICENSE" "$stage/" 2>/dev/null || true
git -C "$root" rev-parse --short HEAD > "$stage/VERSION.txt" 2>/dev/null || echo "unknown" > "$stage/VERSION.txt"

# The app's version: project(FlubsoundPro VERSION x.y.z) in CMakeLists.txt.
version=$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt" | head -n 1)
version=${version:-0.0.0}

case "$label" in
    windows)
        iscc=${ISCC:-}
        if [ -z "$iscc" ]; then
            iscc=$(command -v iscc 2>/dev/null || command -v ISCC.exe 2>/dev/null || true)
        fi
        if [ -z "$iscc" ]; then
            for candidate in "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
                if [ -x "$candidate" ]; then iscc=$candidate; break; fi
            done
        fi
        if [ -n "$iscc" ]; then
            # ISCC takes Windows paths; MSYS must not rewrite the /D switches.
            winpath() { if command -v cygpath > /dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
            MSYS2_ARG_CONV_EXCL='*' "$iscc" /Q "/DAppVersion=$version" "/DSourceDir=$(winpath "$(cd "$stage" && pwd)")" \
                "/DOutputDir=$(winpath "$(cd "$out" && pwd)")" "$(winpath "$root/installer/windows/FlubsoundPro.iss")"
        elif [ "${FLUB_REQUIRE_INSTALLER:-0}" = "1" ]; then
            echo "error: Inno Setup (ISCC.exe) not found; set ISCC or install it (choco install innosetup)" >&2
            exit 1
        else
            echo "warning: Inno Setup (ISCC.exe) not found: no installer, only the folder" >&2
        fi
        ;;
    macos)
        (cd "$out" && ditto -c -k --sequesterRsrc --keepParent "Flubsound-$label" "Flubsound-$label.zip")
        if ! bash "$root/installer/macos/make-dmg.sh" "$stage" "$out/Flubsound-$label.dmg" "Flubsound Pro"; then
            echo "warning: the .dmg could not be made; Flubsound-$label.zip has the same contents" >&2
            rm -f "$out/Flubsound-$label.dmg"
        fi
        rm -rf "$stage"
        ;;
    linux)
        cp "$root/installer/linux/flubsound-pro.desktop" "$root/installer/linux/install.sh" "$stage/"
        chmod +x "$stage/install.sh"
        tar -C "$out" -czf "$out/Flubsound-$label.tar.gz" "Flubsound-$label"
        rm -rf "$stage"
        ;;
esac

ls -la "$out"
