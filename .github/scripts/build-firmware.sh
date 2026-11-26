#!/bin/bash
# CI: the firmware half of ../../install.sh from nothing, up to the finished build,
# with settings full of the characters a shell or a parser would mangle. Then checks
# that they are in the image as they were given. /bin/bash, so that a Mac runs it
# under its own bash 3.2, as install.sh must.
set -euo pipefail
cd "$(dirname "$0")/../.."

export AC_WIFI_SSID='Café "Net" 100%'
export AC_WIFI_PASSWORD='p w;x#y&|<>(){}"q'\''\ $HOME `x` %d%% ??/'
export AC_HUB_URL=192.0.2.10 AC_DEVICE_KEY=ci-device-key AC_HOSTNAME=ci-display
export AC_TIMEZONE='<+04>-4' AC_ROTATE_180=1

/bin/bash ./install.sh firmware --yes --no-flash

cd firmware
./pio --version
python3 - <<'PY'
import os, sys
image = open(".pio/build/waveshare7/firmware.bin", "rb").read()
missing = [name for name in ("AC_WIFI_SSID", "AC_WIFI_PASSWORD", "AC_DEVICE_KEY", "AC_HOSTNAME", "AC_TIMEZONE")
           if os.environb[name.encode()] + b"\0" not in image]
if b"http://192.0.2.10:8767\0" not in image:
    missing.append("AC_HUB_URL")
if missing:
    sys.exit("not in firmware.bin as given: " + ", ".join(missing))
print("firmware.bin: %d bytes, every setting in it as given" % len(image))
PY

# A second run keeps the settings and has nothing left to do.
cd ..
/bin/bash ./install.sh firmware --yes --no-flash | tee /tmp/second-run.log
grep -q "Kept your firmware/secrets.ini" /tmp/second-run.log
