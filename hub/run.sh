#!/usr/bin/env bash
# Starts the hub. Settings are environment variables, read from .env next to this
# script when it exists (copy .env.example); see the README for each of them.
set -euo pipefail
cd "$(dirname "$0")"
if [ -f .env ]; then
    set -a
    . ./.env
    set +a
fi
export PYTHONUNBUFFERED=1
exec .venv/bin/uvicorn --factory agent_control.hub:create_app \
    --host "${AGENT_CONTROL_BIND:-0.0.0.0}" --port "${AGENT_CONTROL_PORT:-8767}"
