#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: start-clion-mo2-remote-dev.sh [--dry-run] [--keep-state] [--keep-logs] [--] [SERVER_ARGUMENT ...]

Start the Windows CLion remote-development server in the MO2 Proton prefix,
then open its join URL with the local JetBrains client.

CLion must first be installed into the prefix, for example:
  ./proton.sh /absolute/path/to/CLion-installer.exe

Options:
  --dry-run     Print commands without removing files or starting CLion.
  --keep-state  Keep stale CLion instance and RemoteDev session files.
  --keep-logs   Keep existing CLion and JetBrains daemon logs.

Environment overrides:
  CLION_REMOTE_DEV_SERVER  Unix path to remote-dev-server.exe
  CLION_LLDB_BIN           Unix path to the bundled x64 LLDB bin directory
  CLION_WINE_USER          Prefix user directory (default: steamuser for Proton)
  CLION_CLIENT_BIN         Local JetBrains client launcher
  CLION_PROTON_LAUNCHER    Path to proton.sh
  CLION_PROTON_VERB        Proton verb (default: runinprefix)

The MO2_WINEPREFIX, MO2_PROTON_ROOT, MO2_UMU_RUN, and MO2_GAME_ID overrides
accepted by proton.sh are forwarded automatically.
EOF
}

dry_run=false
clean_state=true
clean_logs=true

while (($# > 0)); do
    case "$1" in
        --dry-run)
            dry_run=true
            shift
            ;;
        --keep-state|--keep-locks)
            clean_state=false
            shift
            ;;
        --keep-logs)
            clean_logs=false
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

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
wine_prefix="${MO2_WINEPREFIX:-/mnt/storage/Lutris/MO2}"
if [[ -n "${CLION_WINE_USER:-}" ]]; then
    wine_user="${CLION_WINE_USER}"
elif [[ -d "${wine_prefix}/drive_c/users/steamuser" ]]; then
    wine_user=steamuser
else
    wine_user="$(id -un)"
fi
proton_launcher="${CLION_PROTON_LAUNCHER:-${script_dir}/proton.sh}"
proton_verb="${CLION_PROTON_VERB:-runinprefix}"
client_bin="${CLION_CLIENT_BIN:-/home/oleksandr/.local/share/JetBrains/Toolbox/apps/clion/bin/jetbrains_client.sh}"

if [[ ! -x "${proton_launcher}" ]]; then
    echo "MO2 Proton launcher is not executable: ${proton_launcher}" >&2
    exit 1
fi

if [[ ! -x "${client_bin}" ]]; then
    echo "JetBrains client launcher is not executable: ${client_bin}" >&2
    exit 1
fi

if [[ -n "${CLION_REMOTE_DEV_SERVER:-}" ]]; then
    remote_dev_server="${CLION_REMOTE_DEV_SERVER}"
else
    mapfile -d '' server_candidates < <(
        find "${wine_prefix}/drive_c/Program Files/JetBrains" \
            -maxdepth 3 -type f -iname remote-dev-server.exe -print0 2>/dev/null \
            | sort -zV
    )

    if ((${#server_candidates[@]} == 0)); then
        echo "CLion remote-dev-server.exe was not found in ${wine_prefix}." >&2
        echo "Install CLion first with:" >&2
        echo "  ${script_dir}/proton.sh /absolute/path/to/CLion-installer.exe" >&2
        echo "Or set CLION_REMOTE_DEV_SERVER to its Unix path." >&2
        exit 1
    fi

    remote_dev_server="${server_candidates[${#server_candidates[@]} - 1]}"
fi

if [[ ! -f "${remote_dev_server}" ]]; then
    echo "CLion remote development server not found: ${remote_dev_server}" >&2
    exit 1
fi

clion_install_dir="$(cd -- "$(dirname -- "${remote_dev_server}")/.." && pwd -P)"
lldb_bin="${CLION_LLDB_BIN:-${clion_install_dir}/bin/lldb/win/x64/bin}"
proton_dbghelp="${wine_prefix}/drive_c/windows/system32/dbghelp.dll"

if [[ ! -f "${lldb_bin}/liblldb.dll" ]]; then
    echo "CLion's bundled liblldb.dll was not found in: ${lldb_bin}" >&2
    echo "Set CLION_LLDB_BIN to the bundled x64 LLDB bin directory." >&2
    exit 1
fi

if [[ ! -f "${proton_dbghelp}" ]]; then
    echo "Proton's dbghelp.dll was not found: ${proton_dbghelp}" >&2
    exit 1
fi

unix_to_wine_path() {
    local unix_path="$1"
    local relative_path

    if [[ "${unix_path}" == "${wine_prefix}/drive_c/"* ]]; then
        relative_path="${unix_path#"${wine_prefix}/drive_c/"}"
        printf 'C:\\%s' "${relative_path//\//\\}"
    else
        printf 'Z:%s' "${unix_path//\//\\}"
    fi
}

lldb_bin_windows="$(unix_to_wine_path "${lldb_bin}")"
missing_minidump_dll='api-ms-win-core-debug-minidump-l1-1-0.dll'

appdata_root="${wine_prefix}/drive_c/users/${wine_user}/AppData"
local_jetbrains="${appdata_root}/Local/JetBrains"
roaming_jetbrains="${appdata_root}/Roaming/JetBrains"
stale_state=()

if [[ "${clean_state}" == true ]]; then
    while IFS= read -r -d '' state_file; do
        stale_state+=("${state_file}")
    done < <(
        find "${local_jetbrains}" "${roaming_jetbrains}" \
            -maxdepth 2 \( -type f -o -type s \) \
            \( -name '*.lock' -o -name '*.port' -o -name '*.pid' -o -name '*.token' \) \
            -print0 \
            2>/dev/null || true
    )

    remote_dev_recent="${local_jetbrains}/RemoteDev/recent"
    if [[ -d "${remote_dev_recent}" ]]; then
        while IFS= read -r -d '' state_file; do
            stale_state+=("${state_file}")
        done < <(
            find "${remote_dev_recent}" -mindepth 1 -maxdepth 1 \
                \( -type f -o -type s \) -print0 2>/dev/null || true
        )
    fi
fi

if [[ "${clean_state}" == true ]]; then
    if ((${#stale_state[@]} > 0)); then
        for state_file in "${stale_state[@]}"; do
            echo "Removing stale state: ${state_file}"
        done
        if [[ "${dry_run}" == false ]]; then
            rm -f -- "${stale_state[@]}"
        fi
    else
        echo "No stale CLion state found for Wine user ${wine_user}."
    fi
else
    echo "Keeping existing CLion state for Wine user ${wine_user}."
fi

log_dirs=()
if [[ "${clean_logs}" == true ]]; then
    while IFS= read -r -d '' log_dir; do
        log_dirs+=("${log_dir}")
    done < <(
        find "${local_jetbrains}" -maxdepth 2 -type d \
            \( -path '*/CLion*/log' -o -path '*/Daemon/logs' \) -print0 \
            2>/dev/null || true
    )

    for log_dir in "${log_dirs[@]}"; do
        echo "Clearing logs: ${log_dir}"
        if [[ "${dry_run}" == false ]]; then
            find "${log_dir}" -mindepth 1 -delete
        fi
    done
fi

echo "Starting ${remote_dev_server}"

if [[ "${dry_run}" == true ]]; then
    printf 'LLDB WINEPATH: %s;Z:\\tmp\\clion-lldb-shim.XXXXXX\n' \
        "${lldb_bin_windows}"
    printf 'LLDB shim: %s -> %s\n' "${missing_minidump_dll}" "${proton_dbghelp}"
    printf 'Command:'
    printf ' %q' env "MO2_PROTON_VERB=${proton_verb}" \
        "WINEPATH=${lldb_bin_windows};Z:\\tmp\\clion-lldb-shim.XXXXXX" \
        "${proton_launcher}" -- "${remote_dev_server}" run "$@"
    printf '\n'
    printf 'Client after join URL: %q thinClient <tcp-url>\n' "${client_bin}"
    exit 0
fi

# CLion starts LLDBFrontend.exe from nativeDebug-plugin, while liblldb.dll and
# its Python runtime live in the separate bundled LLDB directory. Proton also
# lacks the Windows API-set alias used for MiniDumpWriteDump. Add both to the
# inherited Windows search path without modifying the CLion installation.
lldb_shim_dir="$(mktemp -d /tmp/clion-lldb-shim.XXXXXX)"
ln -s "${proton_dbghelp}" "${lldb_shim_dir}/${missing_minidump_dll}"

cleanup_lldb_shim() {
    if [[ -L "${lldb_shim_dir}/${missing_minidump_dll}" ]]; then
        unlink -- "${lldb_shim_dir}/${missing_minidump_dll}"
    fi
    rmdir -- "${lldb_shim_dir}" 2>/dev/null || true
}
trap cleanup_lldb_shim EXIT

lldb_shim_windows="$(unix_to_wine_path "${lldb_shim_dir}")"
lldb_winepath="${lldb_bin_windows};${lldb_shim_windows}"
if [[ -n "${WINEPATH:-}" ]]; then
    lldb_winepath+=";${WINEPATH}"
fi

coproc CLION_SERVER {
    exec env "MO2_PROTON_VERB=${proton_verb}" "WINEPATH=${lldb_winepath}" \
        "${proton_launcher}" -- "${remote_dev_server}" run "$@" 2>&1
}
server_pid="${CLION_SERVER_PID}"
exec {server_output_fd}<&"${CLION_SERVER[0]}"

forward_signal() {
    local signal="$1"
    kill -s "${signal}" "${server_pid}" 2>/dev/null || true
}
trap 'forward_signal INT' INT
trap 'forward_signal TERM' TERM
trap 'forward_signal HUP' HUP

client_started=false
join_link_pattern='Join link:[[:space:]]*(tcp://[^[:space:]]+)'

while IFS= read -r server_line <&"${server_output_fd}"; do
    printf '%s\n' "${server_line}"

    if [[ "${client_started}" == false && "${server_line}" =~ ${join_link_pattern} ]]; then
        server_url="${BASH_REMATCH[1]}"
        echo "Starting JetBrains client with ${server_url}"
        "${client_bin}" thinClient "${server_url}" &
        client_started=true
    fi
done
exec {server_output_fd}<&-

trap - INT TERM HUP

if wait "${server_pid}"; then
    exit 0
else
    exit $?
fi
