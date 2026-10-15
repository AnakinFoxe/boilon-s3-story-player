#!/usr/bin/env python3
"""Establish the BOILON S3 Gen2 GPIO15 power latch from ESP32-S3 ROM.

The GPIO15 sequence only writes volatile RTCIO and SENS registers. Device
detection also reads identity/security state and may apply esptool's standard
volatile watchdog configuration. The tool does not write Flash, NVS, OTA
state, or eFuse. The user must hold the device's top power button until this
tool reports PASS.
"""

import argparse

import esptool
from esptool.cmds import detect_chip


EXPECTED_ESPTOOL_VERSION = "4.12.0"

LP_IOMUX_CLOCK_REG = 0x60008904
RTC_OUT_REG = 0x60008400
RTC_OUT_W1TS_REG = 0x60008404
RTC_OE_REG = 0x6000840C
RTC_OE_W1TS_REG = 0x60008410
RTC_OE_W1TC_REG = 0x60008414
GPIO15_PIN_REG = 0x60008464
GPIO15_PAD_REG = 0x600084C0
RTC_PAD_HOLD_REG = 0x600080D8
GLOBAL_PAD_HOLD_REG = 0x60008088

GPIO15_DATA_MASK = 0x02000000
GPIO15_PAD_HOLD_MASK = 0x00008000
GLOBAL_FORCE_HOLD_MASK = 0x00200000
LP_IOMUX_CLOCK_MASK = 0x80000000
PAD_FUNCTION_MASK = 0x000E0000
PAD_RTC_FUNCTION = 0x00080000
OPEN_DRAIN_MASK = 0x00000004


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Exact /dev/cu.usbmodem... path")
    parser.add_argument(
        "--confirm-volatile-write",
        action="store_true",
        help="Confirm the user is holding the top power button",
    )
    return parser.parse_args()


def establish_power_latch(esp) -> None:
    pad_hold = esp.read_reg(RTC_PAD_HOLD_REG)
    global_hold = esp.read_reg(GLOBAL_PAD_HOLD_REG)
    require(
        (pad_hold & GPIO15_PAD_HOLD_MASK) == 0,
        "GPIO15 individual hold is set; stop without changing it",
    )
    require(
        (global_hold & GLOBAL_FORCE_HOLD_MASK) == 0,
        "global RTC pad hold is set; stop without changing it",
    )

    # Equivalent to rtc_gpio_init(GPIO15), push-pull output, level high.
    # Disable the output first, set its latch high, then enable it last so
    # the power-control line is never actively driven low.
    esp.write_reg(LP_IOMUX_CLOCK_REG, LP_IOMUX_CLOCK_MASK, LP_IOMUX_CLOCK_MASK)
    esp.write_reg(RTC_OE_W1TC_REG, GPIO15_DATA_MASK)
    output_enable = esp.read_reg(RTC_OE_REG)
    require(
        (output_enable & GPIO15_DATA_MASK) == 0,
        "GPIO15 output could not be disabled safely",
    )
    esp.write_reg(GPIO15_PIN_REG, 0, OPEN_DRAIN_MASK)
    esp.write_reg(GPIO15_PAD_REG, PAD_RTC_FUNCTION, PAD_FUNCTION_MASK)
    esp.write_reg(RTC_OUT_W1TS_REG, GPIO15_DATA_MASK)
    esp.write_reg(RTC_OE_W1TS_REG, GPIO15_DATA_MASK)

    clock = esp.read_reg(LP_IOMUX_CLOCK_REG)
    output = esp.read_reg(RTC_OUT_REG)
    output_enable = esp.read_reg(RTC_OE_REG)
    pin = esp.read_reg(GPIO15_PIN_REG)
    pad = esp.read_reg(GPIO15_PAD_REG)

    require((clock & LP_IOMUX_CLOCK_MASK) != 0, "LP IOMUX clock did not enable")
    require(
        (output & GPIO15_DATA_MASK) != 0,
        "GPIO15 output latch is not high",
    )
    require(
        (output_enable & GPIO15_DATA_MASK) != 0,
        "GPIO15 output is not enabled",
    )
    require((pin & OPEN_DRAIN_MASK) == 0, "GPIO15 is not push-pull")
    require(
        (pad & PAD_FUNCTION_MASK) == PAD_RTC_FUNCTION,
        "GPIO15 pad is not on RTC function 0",
    )


def main() -> None:
    args = parse_args()
    require(
        esptool.__version__ == EXPECTED_ESPTOOL_VERSION,
        f"requires esptool {EXPECTED_ESPTOOL_VERSION}, got {esptool.__version__}",
    )
    if not args.confirm_volatile_write:
        raise SystemExit("refusing to run without --confirm-volatile-write")

    with detect_chip(args.port, 115200, "usb_reset") as esp:
        require(esp.CHIP_NAME == "ESP32-S3", f"unexpected chip: {esp.CHIP_NAME}")
        require(not esp.IS_STUB, "expected ESP32-S3 ROM, but a flasher stub is active")
        establish_power_latch(esp)

    print("PASS: ROM established GPIO15 RTC push-pull high; release the power key")


if __name__ == "__main__":
    main()
