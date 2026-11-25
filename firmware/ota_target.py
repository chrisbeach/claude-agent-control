# PlatformIO extra script: adds `pio run -t ota`, which builds firmware.bin and
# pushes it to the display over Wi-Fi with tools/ota_push.py. The target is
# <hostname>.local from secrets.ini unless OTA_HOST is set in the environment.
import os
import subprocess
import sys

Import("env")

tools = os.path.join(env.subst("$PROJECT_DIR"), "tools")
sys.path.insert(0, tools)
import board_settings

script = os.path.join(tools, "ota_push.py")
firmware = "$BUILD_DIR/${PROGNAME}.bin"


def push(target, source, env):
    settings = board_settings.read(os.path.join(env.subst("$PROJECT_DIR"), "secrets.ini"))
    host = os.environ.get("OTA_HOST", settings["hostname"] + ".local")
    cmd = [env.subst("$PYTHONEXE"), script, "--host", host, env.subst(firmware)]
    return subprocess.call(cmd, env={**os.environ, "DEVICE_KEY": settings["device_key"]})


env.AddCustomTarget(
    name="ota",
    dependencies=firmware,
    actions=[push],
    title="OTA upload",
    description="Push firmware.bin to the display over Wi-Fi and wait for it to boot and confirm",
)
