#!/usr/bin/env bash
# Gives a host's probe the Agent SDK, so its sessions are listed by title and not
# only by name: a venv at ~/.local/share/agent-control/venv, which the hub's SSH
# probe uses when it exists. No sudo.
#     ./bootstrap-host.sh laptop [more hosts...]
#
# Needs only python3 (3.10 or later, the SDK's floor) and a way out to PyPI. The
# venv is made without pip and filled with pip's own zipapp, because Ubuntu's
# python3 ships without ensurepip unless python3-venv is installed. A host that
# cannot take it is reported and the rest are still done; restart the hub
# afterwards (systemctl --user restart agent-control, or however you run it) so
# the probes start again under the new interpreter.
set -uo pipefail

setup() {
    cat <<'EOF'
set -e
dir="$HOME/.local/share/agent-control/venv"
if ! "$dir/bin/python" -c "import claude_agent_sdk" 2>/dev/null; then
    if ! python3 -c "import sys; sys.exit(sys.version_info < (3, 10))"; then
        echo "python3 here is $(python3 -c 'import sys; print(sys.version.split()[0])'); the Agent SDK needs 3.10 or later, so this host's sessions stay listed by name"
        exit 3
    fi
    mkdir -p "$(dirname "$dir")"
    python3 -m venv --clear --without-pip "$dir"
    python3 -c "import sys, urllib.request; urllib.request.urlretrieve('https://bootstrap.pypa.io/pip/pip.pyz', sys.argv[1])" "$dir/pip.pyz"
    "$dir/bin/python" "$dir/pip.pyz" install -q --disable-pip-version-check claude-agent-sdk
    rm -f "$dir/pip.pyz"
fi
"$dir/bin/python" -c "import claude_agent_sdk, sys; print('sdk', claude_agent_sdk.__version__, 'python', sys.version.split()[0])"
EOF
}

failed=0
for host in "$@"; do  # every line a host says is printed under its name, ssh's own notices included
    if [ "$host" = "$(hostname -s)" ]; then
        setup | bash -l -s 2>&1 | sed "s/^/$host: /" || failed=1
    else
        setup | ssh "$host" 'bash -l -s' 2>&1 | sed "s/^/$host: /" || failed=1
    fi
done
exit "$failed"
