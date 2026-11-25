#!/usr/bin/env python3
"""The board's settings: secrets.ini read as it is written, checked, and turned into
the C header the firmware is built with (settings_header.py does that at each build).

    board_settings.py check [secrets.ini]        say what is wrong with the file, if anything
    board_settings.py get NAME [secrets.ini]     print one setting
    board_settings.py value NAME                 check one value, read from standard input

A value is everything after the "=" with the spaces at its two ends taken off, and
nothing in it is special: no quoting, no escapes, no comment after it. Stdlib only, so
it runs under PlatformIO's own Python.
"""
import os
import re
import sys

EXAMPLE_KEY = "paste-the-device-key-here"
DEFAULT_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "secrets.ini")
HEADER_NAME = "ac_settings.h"


def _length(low, high, what):
    def check(value):
        size = len(value.encode("utf-8"))
        if not low <= size <= high:
            return "is %d bytes long, and %s is %d to %d" % (size, what, low, high)
    return check


def _hub_url(value):
    if not re.fullmatch(r"https?://[^\s/]+", value):
        return "must be http://address:port (or https://), with no path after it and no spaces"
    return _length(1, 100, "a hub address")(value)


def _device_key(value):
    if value == EXAMPLE_KEY:
        return "is still the example's: paste the contents of ~/.config/agent-control/device_key from the hub's machine"
    if not re.fullmatch(r"[\x21-\x7e]{1,128}", value):
        return "must be 1 to 128 letters, digits and punctuation, with no spaces"


def _hostname(value):
    if not re.fullmatch(r"[A-Za-z0-9]([A-Za-z0-9-]{0,30}[A-Za-z0-9])?", value):
        return "must be 1 to 32 letters, digits and hyphens, with no hyphen at either end"


def _timezone(value):
    if not re.fullmatch(r"[\x21-\x7e]{1,63}", value):
        return "must be a POSIX TZ string such as UTC0 or CET-1CEST,M3.5.0,M10.5.0/3"


def _number(low, high):
    def check(value):
        if not re.fullmatch(r"-?\d+", value) or not low <= int(value) <= high:
            return "must be a whole number from %d to %d" % (low, high)
    return check


# name: (the value when the file leaves it out; None = it must be there, check, is a number)
SETTINGS = {
    "wifi_ssid": (None, _length(1, 32, "a Wi-Fi name"), False),
    "wifi_password": (None, _length(8, 63, "a WPA2 or WPA3 password"), False),
    "hub_url": (None, _hub_url, False),
    "device_key": (None, _device_key, False),
    "hostname": ("agent-display", _hostname, False),
    "timezone": ("UTC0", _timezone, False),
    "rotate_180": ("0", _number(0, 1), True),
    "leds_gpio": ("-1", _number(-1, 48), True),
    "leds_reversed": ("0", _number(0, 1), True),
}


class SettingsError(Exception):
    """What is wrong with the settings, one problem per line."""


def problem(name, value):
    """What is wrong with this value for the setting NAME, or None."""
    return SETTINGS[name][1](value)


def read(path=DEFAULT_PATH):
    """The settings in the file at PATH, checked, with the ones it leaves out filled in."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        raise SettingsError(
            "%s is not there. Either run ../install.sh firmware, which asks for what goes in it,\n"
            "or copy secrets.ini.example to secrets.ini and fill it in." % path)
    except UnicodeDecodeError:
        raise SettingsError("%s is not UTF-8 text." % path)
    values, problems = {}, []
    for number, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line[0] in ";#" or line == "[secrets]":
            continue
        name, equals, value = line.partition("=")
        name, value = name.strip(), value.strip()
        where = "%s line %d" % (os.path.basename(path), number)
        if not equals:
            problems.append("%s: not a comment, and not name = value" % where)
        elif name not in SETTINGS:
            problems.append("%s: there is no setting called %r (there are: %s)" % (where, name, ", ".join(SETTINGS)))
        elif name in values:
            problems.append("%s: %s is set twice" % (where, name))
        else:
            values[name] = value
            wrong = problem(name, value)
            if wrong:
                problems.append("%s: %s %s" % (where, name, wrong))
    for name, (default, _, _) in SETTINGS.items():
        if name not in values:
            if default is None:
                problems.append("%s: %s is missing" % (os.path.basename(path), name))
            values[name] = default
    if problems:
        raise SettingsError("\n".join(problems))
    return values


def c_string(value):
    """VALUE as a C string literal, whatever is in it."""
    out = []
    for byte in value.encode("utf-8"):
        char = chr(byte)
        out.append(char if byte < 128 and char.isalnum() or char in " _-.,:/+=@~^*!#%&()[]{}<>|;$`'" else "\\%03o" % byte)
    return '"%s"' % "".join(out)


def header(values):
    """The C header that gives the firmware these settings, as AC_<NAME> macros."""
    lines = ["/* Made from secrets.ini at each build (tools/board_settings.py): change that file, not this one. */",
             "#pragma once"]
    for name, (_, _, is_number) in SETTINGS.items():
        lines.append("#define AC_%s %s" % (name.upper(), values[name] if is_number else c_string(values[name])))
    return "\n".join(lines) + "\n"


def write_header(values, directory):
    """Writes the header into DIRECTORY, leaving a file that already says the same
    alone so that nothing is recompiled for it. Returns its path."""
    path = os.path.join(directory, HEADER_NAME)
    text = header(values)
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return path
    except (FileNotFoundError, UnicodeDecodeError):
        pass
    os.makedirs(directory, exist_ok=True)
    with open(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w", encoding="utf-8") as f:
        f.write(text)
    return path


def main(argv):
    try:
        if len(argv) in (2, 3) and argv[1] == "check":
            read(*argv[2:])
        elif len(argv) in (3, 4) and argv[1] == "get" and argv[2] in SETTINGS:
            print(read(*argv[3:])[argv[2]])
        elif len(argv) == 3 and argv[1] == "value" and argv[2] in SETTINGS:
            wrong = problem(argv[2], sys.stdin.read().rstrip("\n"))
            if wrong:
                raise SettingsError("it %s" % wrong)
        else:
            sys.exit(__doc__.strip())
    except SettingsError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main(sys.argv)
