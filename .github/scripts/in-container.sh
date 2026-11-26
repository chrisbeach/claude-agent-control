#!/bin/sh
# CI: build-firmware.sh on another distribution. Run as root in its container, with the
# repository at /repo (read-only): installs the two things install.sh asks for (a
# python3 and git), then builds as an ordinary user, as install.sh insists.
set -eu
if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq python3 git ca-certificates >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q python3 git util-linux shadow-utils
elif command -v pacman >/dev/null; then
    pacman -Sy --noconfirm --needed python git >/dev/null
fi
python3 -V
useradd -m ci
cp -r /repo /home/ci/repo
chown -R ci /home/ci/repo
su ci -c /home/ci/repo/.github/scripts/build-firmware.sh
