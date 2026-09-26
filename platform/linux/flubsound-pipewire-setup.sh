#!/usr/bin/env bash
# Flubsound Pro - create / remove the per-strip virtual output devices on Linux.
#
# Creates four null sinks through the PulseAudio protocol (works on PipeWire
# via pipewire-pulse and on plain PulseAudio):
#
#   flubsound_game    "Flubsound Game"    7.1 (games render surround, the Game
#                                             strip folds it to binaural)
#   flubsound_music   "Flubsound Music"   stereo
#   flubsound_chat    "Flubsound Chat"    stereo
#   flubsound_system  "Flubsound System"  stereo
#
# Applications are routed to these sinks (pavucontrol, the desktop's sound
# settings, or Flubsound's own app list); the engine processes each sink's
# ".monitor" source and plays the result on the real device.
#
# The sinks live until the sound server restarts. For persistent sinks use
# platform/linux/pipewire/pipewire.conf.d/90-flubsound-sinks.conf instead
# (see README.md).
#
# Usage: flubsound-pipewire-setup.sh [install|remove|status|print-pa-config]   (default: install)
# The script is idempotent: install skips existing sinks, remove only unloads
# modules that created a flubsound_* sink.

set -euo pipefail

readonly PROG="${0##*/}"

# name | description | channels | channel map
readonly SINKS=(
    "flubsound_game|Flubsound Game|8|front-left,front-right,front-center,lfe,rear-left,rear-right,side-left,side-right"
    "flubsound_music|Flubsound Music|2|front-left,front-right"
    "flubsound_chat|Flubsound Chat|2|front-left,front-right"
    "flubsound_system|Flubsound System|2|front-left,front-right"
)

usage() {
    cat <<EOF
Usage: ${PROG} [install|remove|status|print-pa-config]

  install          create the Flubsound null sinks (default; existing ones are kept)
  remove           unload the Flubsound null sinks (streams fall back to the default sink)
  status           show which Flubsound sinks exist
  print-pa-config  print load-module lines for a persistent ~/.config/pulse/default.pa
                   (classic PulseAudio; on PipeWire use pipewire/pipewire.conf.d instead)
EOF
}

die() {
    printf '%s: %s\n' "${PROG}" "$*" >&2
    exit 1
}

require_server() {
    command -v pactl >/dev/null 2>&1 ||
        die "pactl not found - install 'pulseaudio-utils' (it also drives PipeWire through pipewire-pulse)."
    pactl info >/dev/null 2>&1 ||
        die "cannot connect to the sound server (is PipeWire/pipewire-pulse or PulseAudio running for this user?)."
}

# Module arguments for one sink. sink_properties must reach the module parser as
#   sink_properties="device.description='Flubsound Game'"
# - double quotes around the property list, single quotes around the value -
# which both PulseAudio's modargs and pipewire-pulse accept (a bare value would
# be cut at the space). Prints one argument per line.
module_args() {
    local name="$1" description="$2" channels="$3" channel_map="$4"
    printf '%s\n' \
        "sink_name=${name}" \
        "sink_properties=\"device.description='${description}'\"" \
        "channels=${channels}" \
        "channel_map=${channel_map}" \
        "rate=48000" \
        "format=float32le"
}

# True if a sink with this exact name exists.
sink_exists() {
    local name="$1"
    pactl list short sinks | awk -F '\t' -v n="${name}" '$2 == n { found = 1 } END { exit !found }'
}

# Prints the ids of module-null-sink instances whose arguments name this sink.
module_ids_for() {
    local name="$1"
    pactl list short modules |
        awk -F '\t' -v n="${name}" '$2 == "module-null-sink" && $3 ~ ("(^|[[:space:]])sink_name=" n "([[:space:]]|$)") { print $1 }'
}

install_sinks() {
    local entry name description channels channel_map
    for entry in "${SINKS[@]}"; do
        IFS='|' read -r name description channels channel_map <<<"${entry}"

        if sink_exists "${name}"; then
            printf 'exists   %-18s (%s)\n' "${name}" "${description}"
            continue
        fi

        local -a args=()
        mapfile -t args < <(module_args "${name}" "${description}" "${channels}" "${channel_map}")

        pactl load-module module-null-sink "${args[@]}" >/dev/null ||
            die "could not create ${name}"

        printf 'created  %-18s (%s, %s ch)\n' "${name}" "${description}" "${channels}"
    done

    printf '\nRoute applications to these outputs (pavucontrol "Playback" tab or Flubsound),\n'
    printf 'Flubsound processes <sink>.monitor, e.g. flubsound_game.monitor.\n'
}

remove_sinks() {
    local entry name description channels channel_map id removed
    for entry in "${SINKS[@]}"; do
        IFS='|' read -r name description channels channel_map <<<"${entry}"
        removed=0

        while read -r id; do
            [[ -n "${id}" ]] || continue
            pactl unload-module "${id}" && removed=1
        done < <(module_ids_for "${name}")

        if ((removed)); then
            printf 'removed  %-18s (%s)\n' "${name}" "${description}"
        elif sink_exists "${name}"; then
            printf 'kept     %-18s (not created by pactl - see pipewire.conf.d / default.pa)\n' "${name}"
        else
            printf 'absent   %-18s\n' "${name}"
        fi
    done
}

print_pa_config() {
    local entry name description channels channel_map
    printf '# Flubsound virtual outputs - append to ~/.config/pulse/default.pa\n'
    printf '# (after ".include /etc/pulse/default.pa"), then: pulseaudio -k\n'
    for entry in "${SINKS[@]}"; do
        IFS='|' read -r name description channels channel_map <<<"${entry}"
        local -a args=()
        mapfile -t args < <(module_args "${name}" "${description}" "${channels}" "${channel_map}")
        printf 'load-module module-null-sink %s\n' "${args[*]}"
    done
}

show_status() {
    local entry name description channels channel_map
    for entry in "${SINKS[@]}"; do
        IFS='|' read -r name description channels channel_map <<<"${entry}"
        if sink_exists "${name}"; then
            printf 'present  %-18s (%s, %s ch)\n' "${name}" "${description}" "${channels}"
        else
            printf 'absent   %-18s (%s)\n' "${name}" "${description}"
        fi
    done
}

main() {
    local command="${1:-install}"

    case "${command}" in
        -h | --help | help)
            usage
            return 0
            ;;
        print-pa-config)
            print_pa_config
            return 0
            ;;
        install | remove | status) ;;
        *)
            usage >&2
            return 2
            ;;
    esac

    require_server

    case "${command}" in
        install) install_sinks ;;
        remove) remove_sinks ;;
        status) show_status ;;
        *) return 2 ;;
    esac
}

main "$@"
