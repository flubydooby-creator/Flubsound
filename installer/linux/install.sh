#!/usr/bin/env bash
# Flubsound Pro - install the Linux test build for the current user (docs/11
# E54). Run it from the unpacked Flubsound-linux folder:
#
#   ./install.sh               install / update
#   ./install.sh --uninstall   remove what install.sh installed
#
# Installs, with no root rights needed:
#   ~/.local/lib/flubsound-pro/Flubsound Pro          the app
#   ~/.local/bin/flubsound-pro                        a link to it
#   ~/.local/share/applications/flubsound-pro.desktop the menu entry
#   ~/.vst3/Flubsound FX.vst3                         the VST3 plug-in
# Settings, presets and logs (~/.config/Flubsound) are never touched.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
lib="$HOME/.local/lib/flubsound-pro"
bin="$HOME/.local/bin/flubsound-pro"
desktop="$data/applications/flubsound-pro.desktop"
vst3="$HOME/.vst3/Flubsound FX.vst3"

refresh_menu() {
    command -v update-desktop-database > /dev/null && update-desktop-database "$data/applications" 2> /dev/null || true
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$lib" "$vst3"
    rm -f "$bin" "$desktop"
    refresh_menu
    echo "Flubsound Pro removed (settings in ~/.config/Flubsound kept)."
    exit 0
fi

[ -x "$here/Flubsound Pro" ] || { echo "install.sh: run it from the unpacked Flubsound-linux folder" >&2; exit 2; }

mkdir -p "$lib" "$(dirname "$bin")" "$(dirname "$desktop")"
install -m 755 "$here/Flubsound Pro" "$lib/Flubsound Pro"
for f in TESTING.txt AUTHORS.md VERSION.txt LICENSE; do
    if [ -f "$here/$f" ]; then install -m 644 "$here/$f" "$lib/$f"; fi
done
ln -sf "$lib/Flubsound Pro" "$bin"

# The menu entry starts the link by its full path (~/.local/bin is not on
# every PATH), quoted as the desktop entry spec asks (a home folder may
# contain spaces).
sed -e "s|^Exec=.*|Exec=\"$bin\"|" "$here/flubsound-pro.desktop" > "$desktop"
chmod 644 "$desktop"

if [ -d "$here/Plug-ins/VST3/Flubsound FX.vst3" ]; then
    mkdir -p "$(dirname "$vst3")"
    rm -rf "$vst3"
    cp -R "$here/Plug-ins/VST3/Flubsound FX.vst3" "$vst3"
fi
refresh_menu

echo "Flubsound Pro installed: start it from the applications menu or run $bin."
if [ -d "$vst3" ]; then echo "VST3 plug-in: $vst3"; fi
