#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  mo2-proton.sh [--in-prefix|--initialize] [--dry-run] [--] PROGRAM [ARG ...]
  mo2-proton.sh [--in-prefix|--initialize] --shell

Run a Windows program in the /mnt/storage/Lutris/MO2 Proton prefix through
the same UMU/GE-Proton stack used by Lutris.

Modes:
  --initialize  Fully initialize/update the prefix before running (default).
  --in-prefix   Join an already initialized/running prefix without updating it.
  --shell       Open a Linux shell with prun and prun_in_prefix helpers.
  --dry-run     Print the resulting command without executing it.

Examples:
  ./mo2-proton.sh cmd.exe
  ./mo2-proton.sh cmd.exe /c ver
  ./mo2-proton.sh /absolute/path/to/setup.exe
  ./mo2-proton.sh --in-prefix regedit.exe
  ./mo2-proton.sh --shell

Environment overrides:
  MO2_WINEPREFIX   Prefix/compat-data root
  MO2_PROTON_ROOT  Proton installation directory
  MO2_UMU_RUN      Path to umu-run
  MO2_GAME_ID      UMU game ID (default: umu-default)
  MO2_PROTON_VERB  Proton verb (overridden by mode flags)
EOF
}

dry_run=false
open_shell=false
proton_verb="${MO2_PROTON_VERB:-waitforexitandrun}"

while (($# > 0)); do
    case "$1" in
        --initialize)
            proton_verb=waitforexitandrun
            shift
            ;;
        --in-prefix)
            proton_verb=runinprefix
            shift
            ;;
        --shell)
            open_shell=true
            shift
            ;;
        --dry-run)
            dry_run=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

wine_prefix="${MO2_WINEPREFIX:-/mnt/storage/Lutris/MO2}"
proton_root="${MO2_PROTON_ROOT:-/home/oleksandr/.local/share/Steam/compatibilitytools.d/GE-Proton11-6-x86_64}"
umu_run="${MO2_UMU_RUN:-/home/oleksandr/.local/share/lutris/runtime/umu/umu-run}"
game_id="${MO2_GAME_ID:-umu-default}"

if [[ ! -d "${wine_prefix}/drive_c" ]]; then
    echo "Proton prefix not found: ${wine_prefix}" >&2
    exit 1
fi

if [[ ! -x "${proton_root}/proton" ]]; then
    echo "Proton installation not found: ${proton_root}" >&2
    exit 1
fi

if [[ ! -x "${umu_run}" ]]; then
    echo "UMU launcher is not executable: ${umu_run}" >&2
    exit 1
fi

export WINEPREFIX="${wine_prefix}"
export STEAM_COMPAT_DATA_PATH="${wine_prefix}"
export PROTONPATH="${proton_root}"
export GAMEID="${game_id}"
export PROTON_VERB="${proton_verb}"

if [[ "${open_shell}" == true ]]; then
    if (($# > 0)); then
        echo "--shell does not accept a program." >&2
        usage >&2
        exit 2
    fi

    export MO2_UMU_RUN="${umu_run}"

    prun() {
        command "${MO2_UMU_RUN}" "$@"
    }

    prun_in_prefix() {
        PROTON_VERB=runinprefix command "${MO2_UMU_RUN}" "$@"
    }

    export -f prun prun_in_prefix

    if [[ "${dry_run}" == true ]]; then
        printf 'Environment: WINEPREFIX=%q PROTONPATH=%q GAMEID=%q PROTON_VERB=%q\n' \
            "${WINEPREFIX}" "${PROTONPATH}" "${GAMEID}" "${PROTON_VERB}"
        echo 'Command: bash --noprofile --norc -i'
        exit 0
    fi

    echo "MO2 Proton shell (${PROTONPATH##*/}, verb: ${PROTON_VERB})"
    echo 'Use: prun PROGRAM [ARG ...]'
    echo 'Use while MO2/game is running: prun_in_prefix PROGRAM [ARG ...]'
    exec bash --noprofile --norc -i
fi

if (($# == 0)); then
    echo "No Windows program specified." >&2
    usage >&2
    exit 2
fi

if [[ "${dry_run}" == true ]]; then
    printf 'Environment: WINEPREFIX=%q PROTONPATH=%q GAMEID=%q PROTON_VERB=%q\n' \
        "${WINEPREFIX}" "${PROTONPATH}" "${GAMEID}" "${PROTON_VERB}"
    printf 'Command:'
    printf ' %q' "${umu_run}" "$@"
    printf '\n'
    exit 0
fi

exec "${umu_run}" "$@"
