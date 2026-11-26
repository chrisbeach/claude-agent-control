"""Tests for board_settings.py: python3 -m unittest test_board_settings (stdlib only)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import board_settings

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.join(HERE, "..", "secrets.ini.example")
AWKWARD = "p w;x#y&|<>(){}\"q'\\ $HOME `x` %d%% ??/ café ☕"
GOOD = """[secrets]
wifi_ssid = My Network
wifi_password = %s
hub_url = http://192.0.2.10:8767
device_key = abc123
"""


class BoardSettingsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)
        self.path = os.path.join(self.dir, "secrets.ini")

    def read(self, text):
        with open(self.path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        return board_settings.read(self.path)

    def problems(self, text):
        with self.assertRaises(board_settings.SettingsError) as raised:
            self.read(text)
        return str(raised.exception)

    def test_the_example_is_complete_but_for_the_device_key(self):
        with open(EXAMPLE, encoding="utf-8") as f:
            example = f.read()
        self.assertIn("device_key is still the example's", self.problems(example))
        values = self.read(example.replace(board_settings.EXAMPLE_KEY, "abc123"))
        self.assertEqual(set(values), set(board_settings.SETTINGS))

    def test_a_value_is_read_as_it_is_written(self):
        values = self.read(GOOD % AWKWARD)
        self.assertEqual(values["wifi_password"], AWKWARD)
        self.assertEqual(values["wifi_ssid"], "My Network")

    def test_what_is_left_out_gets_its_default(self):
        values = self.read(GOOD % "12345678")
        self.assertEqual((values["hostname"], values["timezone"], values["leds_gpio"]), ("agent-display", "UTC0", "-1"))

    def test_windows_line_ends_and_a_byte_order_mark(self):
        values = self.read("﻿" + (GOOD % "12345678").replace("\n", "\r\n"))
        self.assertEqual(values["wifi_password"], "12345678")

    def test_every_problem_is_reported_with_its_line(self):
        text = self.problems(
            "wifi_ssid = a\nwifi_ssid = b\nbogus = 1\nnonsense\nwifi_password = short\n"
            "hub_url = 192.0.2.10:8767\nleds_gpio = 99\nhostname = -x\ntimezone = Europe London\n")
        for expected in ("line 2: wifi_ssid is set twice", "line 3: there is no setting called 'bogus'",
                         "line 4: not a comment", "line 5: wifi_password is 5 bytes long", "line 6: hub_url must be",
                         "line 7: leds_gpio must be", "line 8: hostname must be", "line 9: timezone must be",
                         "device_key is missing"):
            self.assertIn(expected, text)

    def test_a_missing_file_says_what_to_do(self):
        with self.assertRaises(board_settings.SettingsError) as raised:
            board_settings.read(os.path.join(self.dir, "none.ini"))
        self.assertIn("install.sh firmware", str(raised.exception))

    def test_lengths_are_counted_in_bytes(self):
        self.assertIsNone(board_settings.problem("wifi_ssid", "x" * 32))
        self.assertIsNotNone(board_settings.problem("wifi_ssid", "x" * 33))
        self.assertIsNotNone(board_settings.problem("wifi_ssid", "é" * 17))
        self.assertIsNone(board_settings.problem("wifi_password", "x" * 63))
        self.assertIsNotNone(board_settings.problem("wifi_password", "x" * 64))

    def test_a_quoted_time_zone_name_is_allowed(self):
        self.assertIsNone(board_settings.problem("timezone", "<+04>-4"))

    def test_the_header_is_only_rewritten_when_it_changes(self):
        values = self.read(GOOD % AWKWARD)
        path = board_settings.write_header(values, os.path.join(self.dir, "out"))
        os.utime(path, (1, 1))
        board_settings.write_header(values, os.path.join(self.dir, "out"))
        self.assertEqual(os.path.getmtime(path), 1)
        values["hostname"] = "other"
        board_settings.write_header(values, os.path.join(self.dir, "out"))
        self.assertNotEqual(os.path.getmtime(path), 1)

    @unittest.skipUnless(shutil.which("cc"), "needs a C compiler")
    def test_a_c_compiler_reads_the_header_back_as_it_was_written(self):
        values = self.read((GOOD % AWKWARD) + "leds_gpio = 6\n")
        board_settings.write_header(values, self.dir)
        source = os.path.join(self.dir, "t.c")
        with open(source, "w") as f:
            f.write('#include <stdio.h>\n#include "ac_settings.h"\n'
                    'int main(void) { fputs(AC_WIFI_PASSWORD, stdout); return AC_LEDS_GPIO + AC_ROTATE_180; }\n')
        program = os.path.join(self.dir, "t")
        subprocess.check_call(["cc", "-Wall", "-Werror", "-o", program, source])
        result = subprocess.run([program], stdout=subprocess.PIPE)
        self.assertEqual(result.stdout.decode("utf-8"), AWKWARD)
        self.assertEqual(result.returncode, 6)

    def test_the_command_line(self):
        self.read(GOOD % AWKWARD)
        script = os.path.join(HERE, "board_settings.py")
        run = lambda *args, **kw: subprocess.run([sys.executable, script] + list(args), stdout=subprocess.PIPE,
                                                 stderr=subprocess.PIPE, **kw)
        self.assertEqual(run("check", self.path).returncode, 0)
        self.assertEqual(run("get", "wifi_password", self.path).stdout.decode("utf-8").rstrip("\n"), AWKWARD)
        self.assertEqual(run("value", "wifi_password", input=AWKWARD.encode("utf-8")).returncode, 0)
        short = run("value", "wifi_password", input=b"short")
        self.assertEqual(short.returncode, 1)
        self.assertIn(b"5 bytes long", short.stderr)


if __name__ == "__main__":
    unittest.main()
