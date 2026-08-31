#!/usr/bin/env bash
# Install (or reinstall) the AI-Passport BLE microphone bridge as a macOS
# LaunchAgent, so island_agent.py recv-ble starts at login and stays running.
#
#   tools/install-mic-agent.sh            # install + start
#   tools/install-mic-agent.sh --uninstall
#
# Logs go to /tmp/aipassport-mic.log. The agent auto-reconnects to the device;
# launchd auto-restarts the agent itself if it ever exits.
set -euo pipefail

LABEL="com.aipassport.mic"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${HOME}/Library/LaunchAgents/${LABEL}.plist"
domain="gui/$(id -u)"

if [[ "${1:-}" == "--uninstall" ]]; then
    launchctl bootout "${domain}/${LABEL}" 2>/dev/null || true
    rm -f "${dest}"
    echo "Uninstalled ${LABEL}."
    exit 0
fi

# Pick a Python that has the deps (bleak, sounddevice, numpy). Prefer an
# explicit PYTHON override, else the Homebrew 3.11 known to work here, else PATH.
py="${PYTHON:-}"
if [[ -z "${py}" ]]; then
    for cand in /opt/homebrew/opt/python@3.11/bin/python3.11 "$(command -v python3 || true)"; do
        if [[ -x "${cand}" ]] && "${cand}" -c 'import bleak, sounddevice, numpy' 2>/dev/null; then
            py="${cand}"; break
        fi
    done
fi
if [[ -z "${py}" ]]; then
    echo "ERROR: no python with bleak+sounddevice+numpy found." >&2
    echo "  pip install bleak sounddevice numpy pyautogui, or set PYTHON=..." >&2
    exit 1
fi

mkdir -p "${HOME}/Library/LaunchAgents"
sed -e "s#__PYTHON__#${py}#g" -e "s#__REPO__#${repo_root}#g" \
    "${repo_root}/tools/${LABEL}.plist" > "${dest}"

launchctl bootout "${domain}/${LABEL}" 2>/dev/null || true
launchctl bootstrap "${domain}" "${dest}"
launchctl enable "${domain}/${LABEL}"

echo "Installed ${LABEL} using ${py}."
echo "  status: launchctl print ${domain}/${LABEL} | grep state"
echo "  logs:   tail -f /tmp/aipassport-mic.log"
echo "  stop:   tools/install-mic-agent.sh --uninstall"
