#!/usr/bin/env bash
# Opens Claude Code in ~/dev (or ~), for claude-terminal.desktop.
export PATH="$HOME/.local/bin:$HOME/bin:/usr/local/bin:$PATH"  # a desktop launch may not have your shell's PATH
cd "$HOME/dev" 2>/dev/null || cd "$HOME" || exit 1
claude
status=$?
if [ "$status" -ne 0 ]; then  # keep the window open so the message can be read
    echo
    echo "claude exited with status $status. Press Enter to close."
    read -r _
fi
