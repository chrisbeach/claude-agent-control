# PlatformIO extra script, run before each build: checks secrets.ini and writes it out
# as ac_settings.h in the build directory, which the sources include. The values go
# from the file to the compiler as C strings, so any character in a Wi-Fi name or
# password arrives as it was typed. tools/board_settings.py has the rules.
import os
import sys

Import("env")

project = env.subst("$PROJECT_DIR")
sys.path.insert(0, os.path.join(project, "tools"))
import board_settings

# sdkconfig.<env> is made from sdkconfig.defaults by the first build and then kept, so
# it goes stale when the defaults change (a git pull): it is made again from them.
sdkconfig = os.path.join(project, "sdkconfig." + env.subst("$PIOENV"))
defaults = os.path.join(project, "sdkconfig.defaults")
if os.path.isfile(sdkconfig) and os.path.getmtime(defaults) > os.path.getmtime(sdkconfig):
    print("sdkconfig.defaults has changed: making %s from it again" % os.path.basename(sdkconfig))
    os.remove(sdkconfig)

directory = os.path.join(env.subst("$BUILD_DIR"), "settings")
env.Append(CPPPATH=[directory])
if not env.IsCleanTarget():
    try:
        board_settings.write_header(board_settings.read(os.path.join(project, "secrets.ini")), directory)
    except board_settings.SettingsError as error:
        sys.stderr.write("\nThe board's settings:\n%s\n\n" % error)
        env.Exit(1)
