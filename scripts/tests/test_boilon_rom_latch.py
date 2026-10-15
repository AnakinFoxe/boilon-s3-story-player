import importlib.util
import sys
import types
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).parents[1] / "boilon" / "rom_latch_gpio15.py"


def load_module():
    fake_esptool = types.ModuleType("esptool")
    fake_esptool.__version__ = "4.12.0"
    fake_cmds = types.ModuleType("esptool.cmds")
    fake_cmds.detect_chip = object

    saved = {
        name: sys.modules.get(name)
        for name in ("esptool", "esptool.cmds")
    }
    sys.modules["esptool"] = fake_esptool
    sys.modules["esptool.cmds"] = fake_cmds
    try:
        spec = importlib.util.spec_from_file_location("boilon_rom_latch", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        for name, previous in saved.items():
            if previous is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous


class FakeEsp:
    def __init__(self, reads):
        self.reads = dict(reads)
        self.writes = []

    def read_reg(self, address):
        value = self.reads[address]
        if isinstance(value, list):
            return value.pop(0)
        return value

    def write_reg(self, address, value, mask=0xFFFFFFFF):
        self.writes.append((address, value, mask))

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False


class BoilonRomLatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def passing_reads(self):
        m = self.module
        return {
            m.RTC_PAD_HOLD_REG: 0,
            m.GLOBAL_PAD_HOLD_REG: 0,
            m.LP_IOMUX_CLOCK_REG: m.LP_IOMUX_CLOCK_MASK,
            m.RTC_OUT_REG: m.GPIO15_DATA_MASK,
            m.RTC_OE_REG: [0, m.GPIO15_DATA_MASK],
            m.GPIO15_PIN_REG: 0,
            m.GPIO15_PAD_REG: 0x40000000 | m.PAD_RTC_FUNCTION,
        }

    def test_exact_safe_write_sequence(self):
        m = self.module
        esp = FakeEsp(self.passing_reads())

        m.establish_power_latch(esp)

        self.assertEqual(
            esp.writes,
            [
                (m.LP_IOMUX_CLOCK_REG, m.LP_IOMUX_CLOCK_MASK, m.LP_IOMUX_CLOCK_MASK),
                (m.RTC_OE_W1TC_REG, m.GPIO15_DATA_MASK, 0xFFFFFFFF),
                (m.GPIO15_PIN_REG, 0, m.OPEN_DRAIN_MASK),
                (m.GPIO15_PAD_REG, m.PAD_RTC_FUNCTION, m.PAD_FUNCTION_MASK),
                (m.RTC_OUT_W1TS_REG, m.GPIO15_DATA_MASK, 0xFFFFFFFF),
                (m.RTC_OE_W1TS_REG, m.GPIO15_DATA_MASK, 0xFFFFFFFF),
            ],
        )

    def test_individual_hold_stops_before_any_write(self):
        m = self.module
        reads = self.passing_reads()
        reads[m.RTC_PAD_HOLD_REG] = m.GPIO15_PAD_HOLD_MASK
        esp = FakeEsp(reads)

        with self.assertRaisesRegex(RuntimeError, "individual hold"):
            m.establish_power_latch(esp)

        self.assertEqual(esp.writes, [])

    def test_failed_output_disable_stops_before_pin_or_latch_changes(self):
        m = self.module
        reads = self.passing_reads()
        reads[m.RTC_OE_REG] = [m.GPIO15_DATA_MASK]
        esp = FakeEsp(reads)

        with self.assertRaisesRegex(RuntimeError, "disabled safely"):
            m.establish_power_latch(esp)

        self.assertEqual(
            esp.writes,
            [
                (m.LP_IOMUX_CLOCK_REG, m.LP_IOMUX_CLOCK_MASK, m.LP_IOMUX_CLOCK_MASK),
                (m.RTC_OE_W1TC_REG, m.GPIO15_DATA_MASK, 0xFFFFFFFF),
            ],
        )

    def test_global_hold_stops_before_any_write(self):
        m = self.module
        reads = self.passing_reads()
        reads[m.GLOBAL_PAD_HOLD_REG] = m.GLOBAL_FORCE_HOLD_MASK
        esp = FakeEsp(reads)

        with self.assertRaisesRegex(RuntimeError, "global RTC pad hold"):
            m.establish_power_latch(esp)

        self.assertEqual(esp.writes, [])

    def test_wrong_chip_stops_before_gpio_register_access(self):
        m = self.module
        esp = FakeEsp({})
        esp.CHIP_NAME = "ESP32-C3"
        esp.IS_STUB = False
        args = types.SimpleNamespace(port="/dev/cu.test", confirm_volatile_write=True)

        with mock.patch.object(m, "parse_args", return_value=args), mock.patch.object(
            m, "detect_chip", return_value=esp
        ), self.assertRaisesRegex(RuntimeError, "unexpected chip"):
            m.main()

        self.assertEqual(esp.writes, [])

    def test_active_stub_stops_before_gpio_register_access(self):
        m = self.module
        esp = FakeEsp({})
        esp.CHIP_NAME = "ESP32-S3"
        esp.IS_STUB = True
        args = types.SimpleNamespace(port="/dev/cu.test", confirm_volatile_write=True)

        with mock.patch.object(m, "parse_args", return_value=args), mock.patch.object(
            m, "detect_chip", return_value=esp
        ), self.assertRaisesRegex(RuntimeError, "flasher stub"):
            m.main()

        self.assertEqual(esp.writes, [])


if __name__ == "__main__":
    unittest.main()
