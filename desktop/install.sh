#!/usr/bin/env bash
# Installs the desktop side of Agent Control for the current user, on a machine
# with a GNOME desktop whose Claude Code sessions the display shows. No sudo.
#
#   desktop/install.sh                            install, or update in place
#   desktop/install.sh --terminal 'alacritty -e'  the "+" opens this terminal (default: ptyxis -s --)
#   desktop/install.sh --uninstall                remove everything it installed
#
# It installs:
#   the Agent Control GNOME Shell extension, which raises and marks windows;
#   ~/.local/bin/agent-control-window, the helper the hub runs;
#   the Antonio font the mark's tab is lettered in;
#   the agent-control-new launcher for the display's "+".
# gnome-shell loads extension code only at login: log out and back in once after
# installing or updating.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UUID="agent-control@chrisbeach.github.io"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
EXT="$DATA/gnome-shell/extensions/$UUID"
FONT_SRC="$HERE/../firmware/shared/fonts/Antonio-Bold.ttf"
FONT="$DATA/fonts/Antonio-Bold.ttf"
BIN="$HOME/.local/bin/agent-control-window"
LAUNCHER="$DATA/applications/agent-control-new.desktop"
GSETTINGS=/usr/bin/gsettings   # by path: a conda build first on PATH has no GNOME schemas

terminal="ptyxis -s --"
uninstall=0
while [ $# -gt 0 ]; do
    case "$1" in
        --terminal) [ $# -ge 2 ] || { echo "--terminal takes a command, e.g. --terminal 'alacritty -e'" >&2; exit 2; }
                    terminal="$2"; shift 2 ;;
        --uninstall) uninstall=1; shift ;;
        *) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
    esac
done

# Adds or removes the extension in org.gnome.shell enabled-extensions. Done with
# gsettings rather than `gnome-extensions enable`, which refuses an extension the
# running shell has not seen yet (it looks for new ones only at login).
set_enabled() {
    [ -x "$GSETTINGS" ] || { echo "gsettings not found: enable $UUID in the Extensions app after logging in again"; return; }
    local now new
    now="$("$GSETTINGS" get org.gnome.shell enabled-extensions)"
    new="$(/usr/bin/python3 - "$now" "$UUID" "$1" <<'PY'
import ast, sys
now, uuid, on = sys.argv[1], sys.argv[2], sys.argv[3] == "on"
now = now.removeprefix("@as ")
current = ast.literal_eval(now) if now.strip() else []
current = [u for u in current if u != uuid] + ([uuid] if on else [])
print("[" + ", ".join(repr(u) for u in current) + "]")
PY
)"
    "$GSETTINGS" set org.gnome.shell enabled-extensions "$new"
}

if [ "$uninstall" = 1 ]; then
    set_enabled off
    rm -rf "$EXT"
    rm -f "$BIN" "$LAUNCHER"
    [ -x /usr/bin/update-desktop-database ] && /usr/bin/update-desktop-database "$DATA/applications" || true
    echo "Removed the Agent Control extension, $BIN and $LAUNCHER."
    echo "Left $FONT in place (other programs may use it); delete it by hand if you like."
    echo "gnome-shell keeps running the extension until you log out."
    exit 0
fi

if ! command -v gnome-shell >/dev/null 2>&1; then
    echo "gnome-shell is not installed here: this is for a GNOME desktop." >&2
    exit 1
fi

# The extension
rm -rf "$EXT"
mkdir -p "$(dirname "$EXT")"
cp -r "$HERE/extension/$UUID" "$EXT"
set_enabled on
echo "Installed the extension in $EXT and enabled it"

# The font for the tab's lettering (the display's own)
if [ -f "$FONT_SRC" ]; then
    mkdir -p "$(dirname "$FONT")"
    install -m 644 "$FONT_SRC" "$FONT"
    command -v fc-cache >/dev/null 2>&1 && fc-cache -f "$(dirname "$FONT")" >/dev/null 2>&1 || true
    echo "Installed $FONT"
else
    echo "No $FONT_SRC: the tab will be lettered in the desktop's sans-serif"
fi

# The helper
mkdir -p "$(dirname "$BIN")"
install -m 755 "$HERE/agent-control-window" "$BIN"
echo "Installed $BIN"

# The "+" launcher. NoDisplay: it is for the hub, not the app grid.
first="${terminal%% *}"
mkdir -p "$(dirname "$LAUNCHER")"
cat > "$LAUNCHER" <<EOF
[Desktop Entry]
Type=Application
Name=New Claude Code session (Agent Control)
Comment=The Agent Control display's "+": a terminal running Claude Code, raised and marked on this desktop
Exec=$BIN new --highlight idle "NEW SESSION" --terminal "$terminal"
Icon=utilities-terminal
Terminal=false
NoDisplay=true
EOF
[ -x /usr/bin/update-desktop-database ] && /usr/bin/update-desktop-database "$DATA/applications" || true
echo "Installed $LAUNCHER (terminal: $terminal)"
if ! command -v "$first" >/dev/null 2>&1; then
    echo "  but $first is not installed: re-run with --terminal '<your terminal> <its run-a-command flag>',"
    echo "  for a terminal that keeps one window per process, e.g. --terminal 'alacritty -e' or --terminal foot"
fi

echo
if [ "$("$GSETTINGS" get org.gnome.shell disable-user-extensions 2>/dev/null)" = "true" ]; then
    echo "User extensions are switched off on this desktop: turn them on in the Extensions app."
fi
if reason="$("$BIN" check 2>&1)"; then
    echo "The extension is already running. If this was an update, log out and back in once to load the new version."
else
    echo "Now log out and back in once: gnome-shell loads extension code only at login."
    echo "(Right now: ${reason#agent-control: })"
fi
cat <<EOF

Then, in the hub's .env (restart the hub afterwards):

  # tap a row: raise its window and mark it
  AGENT_CONTROL_FOCUS_COMMAND='\$HOME/.local/bin/agent-control-window raise {pid} --highlight {state} {title}'

  # the "+": open new sessions on this machine (its name as in AGENT_CONTROL_HOSTS)
  AGENT_CONTROL_NEW_SESSION_HOST=$(uname -n)
  AGENT_CONTROL_NEW_SESSION_LAUNCHER=agent-control-new
EOF
