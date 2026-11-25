#!/usr/bin/env bash
# Sets up Agent Control: the hub on this machine, and the firmware on a Waveshare
# board plugged into it. Run it as yourself, not with sudo.
#
#   ./install.sh             the hub, then the firmware
#   ./install.sh hub         only the hub: on the machine that is always on
#   ./install.sh firmware    only the firmware: on the computer the board is plugged into
#   ./install.sh --yes ...   take the default answer to every yes/no question
#
# It asks for what it needs (the machines to show, your Wi-Fi, the time zone) and
# keeps what is already there: a device key, hub/.env and firmware/secrets.ini are
# never overwritten, so it is safe to run again, for instance after a git pull.
# An answer can be given beforehand in the environment: AC_HOSTS, AC_WIFI_SSID,
# AC_WIFI_PASSWORD, AC_HUB_URL, AC_DEVICE_KEY, AC_HOSTNAME, AC_TIMEZONE,
# AC_ROTATE_180 (0 or 1), AC_UPLOAD_PORT.
#
# Linux and macOS (bash 3.2 or later), any CPU PlatformIO has an ESP32 toolchain
# for. The README's "Manual installation" lists the same steps to do by hand.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB="$HERE/hub"
FW="$HERE/firmware"
OS="$(uname -s)"
THIS="$(uname -n)"
THIS="${THIS%%.*}"   # this machine's name as the hub knows it
KEY_FILE_DEFAULT="$HOME/.config/agent-control/device_key"
BOARD_USB_ID="303A:1001"   # the ESP32-S3's own USB JTAG/serial device
PIO_CORE_VERSION=6.1.19    # the PlatformIO core (pioarduino's) that goes with the platform in firmware/platformio.ini
PIO_HOME="${AGENT_CONTROL_PLATFORMIO:-$HOME/.local/share/agent-control/platformio}"   # as in firmware/pio

if [ -t 1 ]; then
    BOLD=$'\033[1m'; AMBER=$'\033[33m'; PLAIN=$'\033[0m'
else
    BOLD=""; AMBER=""; PLAIN=""
fi
step() { printf '\n%s%s%s\n' "$BOLD" "$*" "$PLAIN"; }
ok()   { printf '  %s\n' "$*"; }
warn() { printf '  %s! %s%s\n' "$AMBER" "$*" "$PLAIN" >&2; }
die()  { printf '\n%s\n' "$*" >&2; exit 1; }
usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

part=all
YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        hub|firmware) part="$1" ;;
        -y|--yes) YES=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
INTERACTIVE=0
if [ -t 0 ] && [ -t 1 ]; then INTERACTIVE=1; fi

# ask VAR "question" [default] [secret]: leaves the answer in VAR. A value already
# there (from the environment) is kept; with nobody to ask, the default is taken.
ask() {
    local var="$1" question="$2" default="${3:-}" secret="${4:-}" answer=""
    [ -z "${!var:-}" ] || return 0
    if [ "$INTERACTIVE" = 1 ]; then
        while [ -z "$answer" ]; do
            if [ -n "$secret" ]; then
                read -r -s -p "  $question: " answer
                echo
            else
                read -r -p "  $question${default:+ [$default]}: " answer
            fi
            answer="${answer:-$default}"
        done
    else
        answer="$default"
        [ -n "$answer" ] || die "Nobody to ask for \"$question\": set $var in the environment, or run this in a terminal."
    fi
    printf -v "$var" '%s' "$answer"
}

# confirm "question" y|n: asks, with that default; --yes or no terminal takes the default.
confirm() {
    local question="$1" default="$2" answer=""
    if [ "$YES" = 0 ] && [ "$INTERACTIVE" = 1 ]; then
        if [ "$default" = y ]; then
            read -r -p "  $question [Y/n] " answer
        else
            read -r -p "  $question [y/N] " answer
        fi
    fi
    case "${answer:-$default}" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

# The first python3 or python3.N at version 3.10 or later: on PATH, then where
# Homebrew puts one (a Mac's own python3 is 3.9).
find_python() {
    local d p IFS=:
    for d in $PATH /opt/homebrew/bin /usr/local/bin; do
        for p in "$d"/python3 "$d"/python3.[0-9]*; do
            case "$p" in *-config) continue ;; esac
            if "$p" -c "import sys; sys.exit(sys.version_info < (3, 10))" 2>/dev/null; then
                printf '%s\n' "$p"
                return 0
            fi
        done
    done
    return 1
}

# How to get a newer Python (or $1, another package) on this kind of machine.
package_hint() {
    local what="${1:-}"
    if [ "$OS" = Darwin ]; then echo "brew install ${what:-python}"
    elif command -v apt-get >/dev/null 2>&1; then echo "sudo apt install ${what:-python3}"
    elif command -v dnf >/dev/null 2>&1; then echo "sudo dnf install ${what:-python3}"
    elif command -v pacman >/dev/null 2>&1; then echo "sudo pacman -S ${what:-python}"
    elif command -v zypper >/dev/null 2>&1; then echo "sudo zypper install ${what:-python3}"
    elif command -v apk >/dev/null 2>&1; then echo "sudo apk add ${what:-python3}"
    else echo "install ${what:-Python} with your package manager"
    fi
}

# make_venv PYTHON DIR: a virtual environment. Debian and Ubuntu leave ensurepip to
# the python3-venv package; without it the venv is made bare and given pip's own
# zipapp, which venv_pip then runs.
make_venv() {
    local py="$1" dir="$2"
    rm -rf "$dir"
    if "$py" -c "import ensurepip" 2>/dev/null; then
        "$py" -m venv "$dir" || { rm -rf "$dir"; die "$py could not make a virtual environment in $dir (its message is above)."; }
    elif ! { "$py" -m venv --without-pip "$dir" \
        && "$py" -c "import sys, urllib.request; urllib.request.urlretrieve('https://bootstrap.pypa.io/pip/pip.pyz', sys.argv[1])" "$dir/pip.pyz" 2>/dev/null; }; then
        rm -rf "$dir"
        die "This Python has no pip to put in a virtual environment, and pip could not be downloaded from bootstrap.pypa.io in its place.
Either: $(package_hint python3-venv)
or check that this machine can reach https://bootstrap.pypa.io. Then run this again."
    fi
}
venv_pip() {
    local dir="$1"
    shift
    if [ -f "$dir/pip.pyz" ]; then
        "$dir/bin/python" "$dir/pip.pyz" "$@"
    else
        "$dir/bin/python" -m pip "$@"
    fi
}

# fetch URL [KEY]: the body on stdout, KEY sent as the bearer token.
# Exit 0 = answered, 4 = refused the key, 1 = no answer.
fetch() {
    "$PY" - "$@" <<'PY'
import sys, urllib.error, urllib.request
request = urllib.request.Request(sys.argv[1])
if len(sys.argv) > 2:
    request.add_header("Authorization", "Bearer " + sys.argv[2])
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # a LAN address: never by way of a proxy
try:
    sys.stdout.write(opener.open(request, timeout=4).read().decode("utf-8", "replace"))
except urllib.error.HTTPError as e:
    sys.exit(4 if e.code in (401, 403) else 1)
except Exception:
    sys.exit(1)
PY
}

# This machine's address on the LAN, as the board would reach it: the first global
# IPv4 address that is not a container bridge or a VPN tunnel.
lan_ip() {
    local ip="" nic
    if [ "$OS" = Darwin ]; then
        for nic in en0 en1 en2 en3; do
            ip="$(ipconfig getifaddr "$nic" 2>/dev/null)" && break
        done
    elif command -v ip >/dev/null 2>&1; then
        ip="$(ip -4 -o addr show scope global 2>/dev/null \
            | awk '$2 !~ /^(docker|br-|veth|virbr|lxc|tun|tap|wg|nordlynx|tailscale|zt)/ { sub(/\/.*/, "", $4); print $4; exit }')"
    fi
    if [ -z "$ip" ]; then  # whichever address the default route leaves from; nothing is sent
        ip="$("$PY" -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect(('192.0.2.1', 9))
print(s.getsockname()[0])" 2>/dev/null || true)"
    fi
    printf '%s' "$ip"
}

# ---------------------------------------------------------------- the hub

# A setting from hub/.env as run.sh will read it; empty when it is not set.
hub_setting() {
    [ -f "$HUB/.env" ] || return 0
    ( set +eu; set -a; . "$HUB/.env" >/dev/null 2>&1; printf '%s' "${!1:-}" )
}

# hub_set KEY [VALUE]: sets KEY in hub/.env, in place of its commented-out example
# line. Without a VALUE that example line is uncommented as it stands.
hub_set() {
    "$PY" - "$HUB/.env" "$@" <<'PY'
import re, sys
path, key = sys.argv[1:3]
lines = open(path).read().splitlines()
at = next((i for i, line in enumerate(lines) if re.match(r"#?\s*%s=" % re.escape(key), line)), None)
if len(sys.argv) > 3:
    new = "%s=%s" % (key, sys.argv[3])
elif at is not None:
    new = lines[at].lstrip("#").lstrip()
else:
    sys.exit("no example line for %s in %s" % (key, path))
if at is None:
    lines.append(new)
else:
    lines[at] = new
open(path, "w").write("\n".join(lines) + "\n")
PY
}

# The device key of a hub set up on this machine; empty when there is none.
local_device_key() {
    local key file
    key="$(hub_setting AGENT_CONTROL_DEVICE_KEY)"
    if [ -z "$key" ]; then
        file="$(hub_setting AGENT_CONTROL_DEVICE_KEY_FILE)"
        key="$(cat "${file:-$KEY_FILE_DEFAULT}" 2>/dev/null || true)"
    fi
    printf '%s' "$key"
}

hub_port() {
    local port
    port="$(hub_setting AGENT_CONTROL_PORT)"
    printf '%s' "${port:-8767}"
}

# The hub's own address for a check from this machine.
hub_local_url() {
    local bind
    bind="$(hub_setting AGENT_CONTROL_BIND)"
    case "$bind" in ""|0.0.0.0) bind=127.0.0.1 ;; esac
    printf 'http://%s:%s' "$bind" "$(hub_port)"
}

# Waits for the hub to answer and for its probes to report, then prints a line per
# machine. Exit 0 = every machine reports, 1 = some do not, 2 = no hub.
hub_report() {
    "$PY" - "$(hub_local_url)/healthz" <<'PY'
import json, sys, time, urllib.request
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
hosts, deadline = None, time.time() + 45
while time.time() < deadline:
    try:
        hosts = json.loads(opener.open(sys.argv[1], timeout=3).read().decode())["hosts"]
        if all(h["up"] for h in hosts.values()):
            break
    except (OSError, ValueError, KeyError):
        pass
    time.sleep(1)
if hosts is None:
    sys.exit(2)
for name, h in hosts.items():
    if h["up"]:
        print("  %s: %d session%s" % (name, h["sessions"], "" if h["sessions"] == 1 else "s"))
    else:
        print("  %s: NOT REPORTING%s" % (name, ": " + h["error"] if h.get("error") else ""))
sys.exit(0 if all(h["up"] for h in hosts.values()) else 1)
PY
}

install_hub() {
    step "The hub: Python packages"
    local vpy="$HUB/.venv/bin/python"
    if ! "$vpy" -c "import sys; sys.exit(sys.version_info < (3, 10))" 2>/dev/null || ! venv_pip "$HUB/.venv" --version >/dev/null 2>&1; then
        make_venv "$PY" "$HUB/.venv"
    fi
    venv_pip "$HUB/.venv" install -q --disable-pip-version-check -r "$HUB/requirements.txt" \
        || die "pip could not install the hub's packages (its message is above)."
    ok "In hub/.venv, on $("$vpy" -V 2>&1)"

    step "The hub: device key and settings"
    if [ ! -f "$HUB/.env" ]; then
        cp "$HUB/.env.example" "$HUB/.env"
        ok "Wrote hub/.env from the example"
    else
        ok "Kept your hub/.env"
    fi
    if [ -z "$(local_device_key)" ]; then
        local key_file
        key_file="$(hub_setting AGENT_CONTROL_DEVICE_KEY_FILE)"
        key_file="${key_file:-$KEY_FILE_DEFAULT}"
        mkdir -p "$(dirname "$key_file")"
        ( umask 077; "$PY" -c "import secrets; print(secrets.token_urlsafe(24))" > "$key_file" )
        ok "Wrote a new device key (the secret the display and the hub share) to $key_file"
    else
        ok "Kept your device key"
    fi

    local hosts host others=""
    hosts="$(hub_setting AGENT_CONTROL_HOSTS)"
    if [ -z "$hosts" ]; then
        ok "Which machines' Claude Code sessions should the display show? This one is $THIS; name"
        ok "any others as you would to ssh, separated by commas, in the order you want them shown."
        ask AC_HOSTS "Machines" "$THIS"
        hosts="$(printf '%s' "$AC_HOSTS" | tr -d ' ')"
        case "$hosts" in *[!A-Za-z0-9._,@-]*) die "A machine name can only have letters, digits, dots, dashes and underscores: $hosts" ;; esac
        hub_set AGENT_CONTROL_HOSTS "$hosts"
        ok "Set AGENT_CONTROL_HOSTS=$hosts in hub/.env"
    else
        ok "Showing the machines in your hub/.env: $hosts"
    fi

    local shown_here=0 reachable=""
    for host in $(printf '%s' "$hosts" | tr ',' ' '); do
        if [ "$host" = "$THIS" ]; then
            shown_here=1
        else
            others="$others $host"
        fi
    done

    if [ -n "$others" ]; then
        step "The hub: the other machines"
        for host in $others; do
            if ssh -n -o BatchMode=yes -o ConnectTimeout=8 "$host" true 2>/dev/null; then
                ok "$host: ssh works"
                reachable="$reachable $host"
            else
                warn "$host: \"ssh $host true\" does not work without a prompt, so the display will show it as OFF."
                warn "  Set up key-based login (ssh-copy-id $host) and accept its host key once by hand."
            fi
        done
        if [ -n "$reachable" ] && confirm "Give them the Claude Agent SDK, for titles and to-do lines? (a venv in their ~/.local/share/agent-control, no sudo)" y; then
            # shellcheck disable=SC2086  # one word per host
            "$HUB/bootstrap-host.sh" $reachable || warn "Those machines' sessions will be listed by name only (see hub/README.md, \"Titles on other machines\")."
        fi
    fi

    if [ "$shown_here" = 1 ]; then
        local claude
        claude="$(command -v claude 2>/dev/null || true)"
        if [ -z "$claude" ] && [ -x "$HOME/.local/bin/claude" ]; then claude="$HOME/.local/bin/claude"; fi
        if [ -z "$claude" ]; then
            warn "The claude CLI is not on PATH or in ~/.local/bin here: install Claude Code, or this machine shows as OFF."
        elif ! "$vpy" - "$claude" <<'PY'
import json, subprocess, sys
try:
    out = subprocess.run([sys.argv[1], "agents", "--json"], capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL).stdout
    sys.exit(not isinstance(json.loads(out), list))
except Exception:
    sys.exit(1)
PY
        then
            warn "\"claude agents --json\" does not print a JSON list here: update Claude Code (claude update), or this machine's sessions cannot be read."
        fi
    fi

    if [ "$shown_here" = 1 ] && command -v gnome-shell >/dev/null 2>&1 && [ -z "$(hub_setting AGENT_CONTROL_FOCUS_COMMAND)" ]; then
        step "The hub: this machine's GNOME desktop"
        ok "desktop/install.sh can make a tap on the display raise that session's window here, and"
        ok "the + open a new session: a GNOME Shell extension and a helper, for you only, no sudo."
        if confirm "Install them?" n; then
            "$HERE/desktop/install.sh" >/dev/null || warn "desktop/install.sh failed: run it by hand to see why."
            hub_set AGENT_CONTROL_FOCUS_COMMAND
            [ -n "$(hub_setting AGENT_CONTROL_NEW_SESSION_HOST)" ] || hub_set AGENT_CONTROL_NEW_SESSION_HOST "$THIS"
            [ -n "$(hub_setting AGENT_CONTROL_NEW_SESSION_LAUNCHER)" ] || hub_set AGENT_CONTROL_NEW_SESSION_LAUNCHER agent-control-new
            ok "Installed, and set in hub/.env. Log out and back in once: gnome-shell loads an extension only at login."
            ok "The + opens Ptyxis; for another terminal see desktop/README.md."
        else
            ok "Skipped. Later: desktop/install.sh, which prints the settings for hub/.env."
        fi
    fi

    step "The hub: running it"
    local unit="$HOME/.config/systemd/user/agent-control.service" started=0
    if [ "$OS" = Linux ] && systemctl --user show-environment >/dev/null 2>&1; then
        if [ -f "$unit" ] || ! fetch "$(hub_local_url)/healthz" >/dev/null; then
            mkdir -p "$(dirname "$unit")"
            awk -v run="ExecStart=\"$HUB/run.sh\"" '/^#/ { next } /^ExecStart=/ { print run; next } { print }' \
                "$HUB/examples/agent-control.service" > "$unit"
            systemctl --user daemon-reload
            systemctl --user enable -q agent-control
            systemctl --user restart agent-control
            ok "Started as a systemd user service (logs: journalctl --user -u agent-control -f)"
            if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" != yes ]; then
                loginctl enable-linger "$(id -un)" 2>/dev/null \
                    || warn "Could not turn on lingering, so the hub stops when you log out: sudo loginctl enable-linger $(id -un)"
            fi
        else
            ok "A hub you run some other way already answers on port $(hub_port): left as it is."
            ok "Restart it yourself so that it reads the settings above."
        fi
        started=1
    else
        ok "No systemd user session here, so it is yours to start and keep running:"
        ok "  $HUB/run.sh"
    fi

    if [ "$started" = 1 ]; then
        local status=0
        hub_report || status=$?
        case "$status" in
            1) warn "Not every machine reports yet (reasons above); $(hub_local_url)/healthz has the latest." ;;
            2) warn "The hub did not answer on $(hub_local_url): journalctl --user -u agent-control -n 30" ;;
        esac
    fi

    local port
    port="$(hub_port)"
    if grep -qs '^ENABLED=yes' /etc/ufw/ufw.conf; then
        warn "This machine's firewall (ufw) is on. Let the display in: sudo ufw allow $port/tcp"
    elif [ "$OS" = Linux ] && systemctl is-active -q firewalld 2>/dev/null; then
        warn "This machine's firewall (firewalld) is on. Let the display in:"
        warn "  sudo firewall-cmd --permanent --add-port=$port/tcp && sudo firewall-cmd --reload"
    fi
}

# ---------------------------------------------------------------- the firmware

# A value from firmware/secrets.ini.
fw_setting() {
    "$PY" -c "
import configparser, sys
c = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=('#', ';'))
c.read(sys.argv[1])
print(c.get('secrets', sys.argv[2], fallback=''))" "$FW/secrets.ini" "$1"
}

# The time zone of this machine as a POSIX TZ string, which is how its zone file ends.
# A zone with no abbreviation has a quoted one there (<+04>-4), which secrets.ini
# cannot carry: it gets a plain name instead (STD-4), the clock never showing it.
posix_timezone() {
    "$PY" -c "
import re
try:
    data = open('/etc/localtime', 'rb').read()
    tz = data.rstrip(b'\n').rsplit(b'\n', 1)[-1].decode('ascii')
    assert data[:4] == b'TZif' and 0 < len(tz) < 60 and all(32 < ord(c) < 127 for c in tz)
    names = iter(('STD', 'DST'))
    print(re.sub('<[^>]*>', lambda quoted: next(names), tz, count=2))
except Exception:
    print('UTC0')"
}

# check_secret WHAT VALUE: stops when VALUE is one firmware/secrets.ini cannot carry
# into the build. Its values pass through PlatformIO's settings parser and a shell
# on their way to the compiler (secrets.ini.example says what survives that).
check_secret() {
    case "$2" in
        *[\'\"\\\$\`\;\&\|\<\>\(\)\{\}]*|" "*|*" "|"#"*|*" #"*)
            die "The $1 has something in it that firmware/secrets.ini cannot carry into the build:
one of ' \" \\ \$ \` ; & | < > ( ) { }, a space at either end, or a # at the start or after a space.
(Built in all the same, the board would not get what you typed, and a Wi-Fi name or password like
that would never join.) Use one without: for Wi-Fi, a guest network with a plainer password, say." ;;
    esac
}

# PlatformIO for this firmware, in a directory of its own: the platform named in
# firmware/platformio.ini builds only with the core it was released with (a later
# core brings a later SCons, which that platform cannot link with), and keeps its
# tools in the core's directory. firmware/pio runs it.
install_pio() {
    local venv="$PIO_HOME/penv"
    if "$venv/bin/python" -c "
import importlib.metadata as m, sys
sys.exit(m.version('pioarduino') != '$PIO_CORE_VERSION')" 2>/dev/null; then
        ok "PlatformIO core $PIO_CORE_VERSION is in $PIO_HOME"
        return
    fi
    if ! "$PY" -c "import ensurepip" 2>/dev/null; then
        die "PlatformIO needs Python's venv module complete with pip, and this Python has it without:
  $(package_hint python3-venv)
then run this again."
    fi
    mkdir -p "$PIO_HOME"
    make_venv "$PY" "$venv"
    venv_pip "$venv" install -q --disable-pip-version-check "pioarduino==$PIO_CORE_VERSION" \
        || die "pip could not install PlatformIO (its message is above)."
    ok "Installed PlatformIO core $PIO_CORE_VERSION in $PIO_HOME (apart from any other PlatformIO here)"
}

# The serial port of a board plugged in here; empty when there is none.
find_board() {
    if [ -n "${AC_UPLOAD_PORT:-}" ]; then
        printf '%s' "$AC_UPLOAD_PORT"
        return
    fi
    "$PIO" device list --json-output 2>/dev/null | "$PY" -c "
import json, sys
try:
    ports = [p['port'] for p in json.load(sys.stdin) if 'VID:PID=$BOARD_USB_ID' in p.get('hwid', '').upper()]
except ValueError:
    ports = []
print(ports[0] if ports else '', end='')"
}

# Lets this user open the board's serial port: PlatformIO's udev rules, which need sudo.
allow_serial() {
    local port="$1" rules
    rules="$("$PIO_HOME/penv/bin/python" -c "
import os, platformio
print(os.path.join(os.path.dirname(platformio.__file__), 'assets', 'system', '99-platformio-udev.rules'))" 2>/dev/null || true)"
    if [ -f "$rules" ] && [ "$INTERACTIVE" = 1 ] && command -v sudo >/dev/null 2>&1 \
        && confirm "You cannot write to $port. Install PlatformIO's udev rules, which let you? (needs sudo)" y; then
        sudo install -m 644 "$rules" /etc/udev/rules.d/99-platformio-udev.rules
        sudo udevadm control --reload-rules
        sudo udevadm trigger
        sleep 2
    fi
    [ -w "$port" ] || die "You cannot write to $port. Either add yourself to the group that owns it and log in again:
  sudo usermod -aG $(stat -c %G "$port" 2>/dev/null || echo dialout) $(id -un)
or install PlatformIO's udev rules (https://docs.platformio.org/en/latest/core/installation/udev-rules.html)
and plug the board in again. Then: ./install.sh firmware"
}

write_secrets() {
    local key hub_url="" ssid_here=""
    ok "The board joins your Wi-Fi itself: a 2.4 GHz network (it has no 5 GHz radio)."
    if command -v nmcli >/dev/null 2>&1; then
        ssid_here="$(nmcli -t -f active,ssid dev wifi 2>/dev/null | sed -n 's/^yes://p' | head -n 1)"
    fi
    ask AC_WIFI_SSID "Wi-Fi network name" "$ssid_here"
    ask AC_WIFI_PASSWORD "Wi-Fi password (not shown)" "" secret
    check_secret "Wi-Fi name" "$AC_WIFI_SSID"
    check_secret "Wi-Fi password" "$AC_WIFI_PASSWORD"

    key="$(local_device_key)"
    if [ -n "$key" ]; then
        hub_url="http://$(lan_ip):$(hub_port)"
        ok "The board must reach the hub on this machine by an address that stays the same"
        ok "(a DHCP reservation on your router is safest)."
    else
        ok "No hub is set up on this machine, so say where yours is. On the hub's machine,"
        ok "the key is in $KEY_FILE_DEFAULT"
    fi
    ask AC_HUB_URL "The hub's address" "$hub_url"
    case "$AC_HUB_URL" in http://*) ;; *) AC_HUB_URL="http://$AC_HUB_URL" ;; esac
    case "$AC_HUB_URL" in http://*:[0-9]*) ;; *) AC_HUB_URL="${AC_HUB_URL%/}:8767" ;; esac
    AC_HUB_URL="${AC_HUB_URL%/}"
    if [ -n "$key" ]; then AC_DEVICE_KEY="${AC_DEVICE_KEY:-$key}"; fi   # this machine's own: not asked for, nor shown
    ask AC_DEVICE_KEY "The hub's device key"
    check_secret "hub address" "$AC_HUB_URL"
    check_secret "device key" "$AC_DEVICE_KEY"

    local status=0
    fetch "$AC_HUB_URL/api/fleet" "$AC_DEVICE_KEY" >/dev/null || status=$?
    case "$status" in
        0) ok "The hub at $AC_HUB_URL answers, and accepts the key" ;;
        4) die "The hub at $AC_HUB_URL refuses that device key: it must be the contents of $KEY_FILE_DEFAULT on the hub's machine." ;;
        *) warn "No answer from a hub at $AC_HUB_URL from here. If the address is right (and the hub is running,"
           warn "with port ${AC_HUB_URL##*:} open in its firewall), carry on; the board will say NO LINK TO THE HUB if not."
           confirm "Carry on with that address?" y || die "Run this again when the hub answers." ;;
    esac

    ask AC_HOSTNAME "The board's name on your network" agent-display
    ask AC_TIMEZONE "Time zone for its clock, as a POSIX TZ string (this machine's is the default)" "$(posix_timezone)"
    check_secret "board's name" "$AC_HOSTNAME"
    check_secret "time zone" "$AC_TIMEZONE"
    if [ -z "${AC_ROTATE_180:-}" ]; then
        AC_ROTATE_180=0
        if confirm "Will your case or stand hold the board with its USB sockets at the top?" n; then AC_ROTATE_180=1; fi
    fi

    ( umask 077
      "$PY" - "$FW/secrets.ini.example" "$FW/secrets.ini" \
          wifi_ssid "$AC_WIFI_SSID" wifi_password "$AC_WIFI_PASSWORD" hub_url "$AC_HUB_URL" device_key "$AC_DEVICE_KEY" \
          hostname "$AC_HOSTNAME" timezone "$AC_TIMEZONE" rotate_180 "$AC_ROTATE_180" <<'PY'
import re, sys
example, path = sys.argv[1:3]
values = dict(zip(sys.argv[3::2], (v.replace("%", "%%") for v in sys.argv[4::2])))  # the file's own escape for %
lines = []
for line in open(example).read().splitlines():
    name = re.match(r"(\w+)\s*=", line)
    lines.append("%s = %s" % (name.group(1), values[name.group(1)]) if name and name.group(1) in values else line)
open(path, "w").write("\n".join(lines) + "\n")
PY
    )
    ok "Wrote firmware/secrets.ini (the status LEDs and the rest are in there to edit)"
}

install_firmware() {
    step "The firmware: PlatformIO"
    command -v git >/dev/null 2>&1 || die "The ESP32 platform PlatformIO builds with needs git: $(package_hint git), then run this again."
    install_pio

    step "The firmware: settings"
    if [ -f "$FW/secrets.ini" ]; then
        ok "Kept your firmware/secrets.ini"
    else
        write_secrets
    fi

    step "The firmware: building"
    if [ ! -d "$PIO_HOME/packages/framework-espidf" ]; then
        ok "This first build downloads ESP-IDF, its compilers and LVGL (about 2 GB, into"
        ok "$PIO_HOME) and takes several minutes."
    fi
    if [ "$FW/sdkconfig.defaults" -nt "$FW/sdkconfig.waveshare7" ] && [ -f "$FW/sdkconfig.waveshare7" ]; then
        rm -f "$FW/sdkconfig.waveshare7"   # generated from the defaults by the first build, and kept: stale once they change
    fi
    ( cd "$FW" && "$PIO" run ) || die "The build failed (its messages are above). firmware/README.md has more on the build."

    step "The firmware: flashing the board over USB"
    local port name
    port="$(find_board)"
    while [ -z "$port" ]; do
        if [ "$INTERACTIVE" = 0 ]; then
            die "The firmware is built, but no board is plugged in here (an Espressif USB JTAG/serial device, $BOARD_USB_ID).
Plug it in with a USB-C data cable and run: ./install.sh firmware"
        fi
        warn "No board found (an Espressif USB JTAG/serial device, $BOARD_USB_ID, in lsusb)."
        if grep -qis microsoft /proc/version; then
            warn "Under WSL, attach it to Linux first: usbipd attach --wsl --busid <its bus id>, from Windows."
        fi
        read -r -p "  Plug it into this computer with a USB-C data cable, then press Enter (Ctrl-C to stop): " _
        port="$(find_board)"
    done
    ok "The board is on $port"
    if [ "$OS" = Linux ] && [ ! -w "$port" ]; then allow_serial "$port"; fi
    ( cd "$FW" && "$PIO" run -t upload --upload-port "$port" ) || die "The upload failed (its messages are above)."

    name="$(fw_setting hostname)"
    step "The firmware: first start"
    ok "Unplug the board and plug it back in: after a USB flash the chip can stay in its"
    ok "download mode (backlight on, screen black). It then shows JOINING WI-FI, CONTACTING THE"
    ok "HUB, and your sessions."
    if [ "$INTERACTIVE" = 1 ]; then
        read -r -p "  Press Enter once it has restarted: " _
        local tries=0 status=""
        while [ "$tries" -lt 20 ] && ! status="$(fetch "http://$name.local/status")"; do
            tries=$((tries + 1))
            sleep 3
        done
        if [ -n "$status" ]; then
            ok "http://$name.local/status answers: the board is on your network."
        else
            warn "http://$name.local/status does not answer from here. If the screen shows your sessions, .local"
            warn "names just do not resolve on this network; if not, see \"If something is wrong\" in the README."
        fi
    fi
    ok "Later firmware goes over Wi-Fi: cd firmware && ./pio run -t ota"
}

# ---------------------------------------------------------------- main

[ "$(id -u)" != 0 ] || die "Run this as yourself, not as root: the hub reads your own Claude Code sessions."
case "$OS" in Linux|Darwin) ;; *) die "This script is for Linux and macOS; on $OS, follow the README's manual installation." ;; esac
PY="$(find_python)" || die "This needs Python 3.10 or later (the hub does, and so does the ESP32 platform PlatformIO builds
with), and there is none on PATH, in /opt/homebrew/bin or in /usr/local/bin.
Get one ($(package_hint)), then run this again."
PIO="$FW/pio"

do_hub=0
do_firmware=0
case "$part" in
    hub) do_hub=1 ;;
    firmware) do_firmware=1 ;;
    all)
        do_hub=1
        if [ "$OS" = Darwin ]; then
            step "Agent Control"
            ok "The hub belongs on a Linux machine that is always on: on a Mac it is untested, and"
            ok "this script would not keep it running. The firmware can be flashed from here."
            confirm "Set up the hub on this Mac all the same?" n || do_hub=0
        fi
        do_firmware=1 ;;
esac

if [ "$do_hub" = 1 ]; then install_hub; fi
if [ "$part" = all ] && [ "$do_hub" = 1 ]; then
    step "The firmware"
    if ! confirm "Is the Waveshare board plugged into this machine (or will be), to build and flash its firmware from here?" y; then
        do_firmware=0
        ok "Then, on the computer you will plug it into: clone this repository and run ./install.sh firmware"
        ok "It will ask for the hub's address, http://$(lan_ip):$(hub_port), and for the device key, which is"
        ok "what this machine has in $KEY_FILE_DEFAULT"
    fi
fi
if [ "$do_firmware" = 1 ]; then install_firmware; fi

step "Done"
ok "Taps, the + button, usage gauges and status LEDs: see \"Optional extras\" in the README."
