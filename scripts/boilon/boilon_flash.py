#!/usr/bin/env python3
"""Install, back up and restore BOILON S3 Gen2 firmware over USB, one command each.

  python3 boilon_flash.py install boilon-ai-player-vX.Y.Z.bin
  python3 boilon_flash.py backup
  python3 boilon_flash.py restore boilon-backup-YYYYMMDD-HHMMSS/flash.bin
  python3 boilon_flash.py poweroff

It runs docs/boilon/FLASHING.md in one USB session and stops at the first
failed check. Each session holds power through GPIO15 with rom_latch_gpio15.py
(keep it next to this file), checks the chip, security, PSRAM, Flash size and
partition table, and ends with a controlled power-off. install checks the file
against the SHA256SUMS next to it, reads the whole Flash twice into a new
backup folder, writes the app into ota_1 and then the 8 KiB boot selection,
and reads both back. backup stops after the backup. restore writes a backup
back as a whole and reads it back. Nothing else is ever written: not the
bootloader, partition table, NVS or assets (except by restore), and the chip
is never erased. poweroff turns off a device left in download mode.

Needs esptool 4.12.0: python3 -m pip install esptool==4.12.0
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import os
import re
import struct
import sys
import time
import zlib
from pathlib import Path

ESPTOOL_VERSION = "4.12.0"
USB_VID, USB_PID = 0x303A, 0x1001  # the ESP32-S3's built-in USB-Serial/JTAG
ROM_BAUD, STUB_BAUD = 115200, 921600
FLASH_SIZE = 0x1000000
SECTOR = 0x1000
PARTITION_OFFSET = 0x8000
OTADATA_OFFSET = 0x11000
OTA1_OFFSET, OTA1_SIZE = 0x400000, 0x3E0000
PSRAM_CAP_8MB = 1  # eFuse PSRAM_CAP of an ESP32-S3 with 8 MB in the package
IMAGE_CHIP_ID_ESP32S3 = 9
APP_DESC_MAGIC = 0xABCD5432
WAIT_SECONDS = 60

# partitions/boilon-s3-gen2.csv, which is also the factory table:
# label, type, subtype, offset, size.
PARTITIONS = (
    ("nvs", 0x01, 0x02, 0x9000, 0x8000),
    ("otadata", 0x01, 0x00, 0x11000, 0x2000),
    ("phy_init", 0x01, 0x01, 0x13000, 0x1000),
    ("ota_0", 0x00, 0x10, 0x20000, 0x3E0000),
    ("ota_1", 0x00, 0x11, 0x400000, 0x3E0000),
    ("assets", 0x01, 0x82, 0x800000, 0x800000),
)
FIRMWARE_NAME = re.compile(r"boilon-ai-player-v([0-9]+\.[0-9]+\.[0-9]+)\.bin")
RELEASE_NAME = re.compile(rb"boilon-ai-player v([0-9]+\.[0-9]+\.[0-9]+)")

CHECKED = "ESP32-S3、8 MB PSRAM、16 MB Flash；Flash 加密和 Secure Boot 都关着"
POWER_OFF_FAILED = ("没能让设备关机：它可能停在了下载模式（屏幕黑着，串口还在）。"
                    "运行 python3 boilon_flash.py poweroff")


def otadata_sector(seq: int, state: int) -> bytes:
    """One otadata sector as ESP-IDF writes it: label unset, CRC-32 over the sequence number."""
    number = seq.to_bytes(4, "little")
    entry = (number + b"\xff" * 20 + state.to_bytes(4, "little") +
             zlib.crc32(number, 0xFFFFFFFF).to_bytes(4, "little"))
    return entry + b"\xff" * (SECTOR - len(entry))


# The same 8 KiB as make_otadata.py and the release's boilon-otadata-ota1.bin:
# the factory entry (sequence 1: ota_0, valid) and sequence 2 selecting ota_1.
OTADATA_OTA1 = otadata_sector(1, 2) + otadata_sector(2, 0xFFFFFFFF)


class Stop(Exception):
    """A failed check; the message says what was found and what to do."""


def require(condition, message: str) -> None:
    if not condition:
        raise Stop(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def padded(size: int) -> int:
    return -(-size // SECTOR) * SECTOR


def listed_sha256(sums: Path, name: str):
    """The SHA-256 that a sha256sum-style file lists for NAME, or None."""
    for line in sums.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].lstrip("*") == name:
            return fields[0].lower()
    return None


def check_firmware(path: Path) -> bytes:
    """The release file, checked against the SHA256SUMS next to it and its own header."""
    name = FIRMWARE_NAME.fullmatch(path.name)
    require(name, f"{path.name}：文件名应是 Release 里的原名 boilon-ai-player-vX.Y.Z.bin")
    require(path.is_file(), f"找不到 {path}")
    data = path.read_bytes()
    sums = path.with_name("SHA256SUMS")
    require(sums.is_file(), f"{path.name} 旁边没有 SHA256SUMS：从同一个 Release 下载，放进同一个文件夹")
    listed = listed_sha256(sums, path.name)
    require(listed, f"SHA256SUMS 里没有 {path.name}：它们不是同一个 Release 的")
    require(listed == sha256(data), f"{path.name} 和 SHA256SUMS 对不上：多半没下载完整，重新下载")
    require(len(data) > 64 and data[0] == 0xE9
            and int.from_bytes(data[12:14], "little") == IMAGE_CHIP_ID_ESP32S3
            and int.from_bytes(data[32:36], "little") == APP_DESC_MAGIC,
            f"{path.name} 不是 ESP32-S3 的 app 镜像")
    require(padded(len(data)) <= OTA1_SIZE, f"{path.name} 太大，放不进 ota_1")
    require(set(RELEASE_NAME.findall(data)) == {name.group(1).encode()},
            f"{path.name} 里的版本名和文件名对不上")
    return data


def partition_entries(table: bytes) -> list:
    """(label, type, subtype, offset, size, flags) per entry; an MD5 entry must match."""
    entries, md5 = [], hashlib.md5()
    for start in range(0, len(table) - 31, 32):
        record = table[start:start + 32]
        if record[:2] == b"\xff\xff":
            return entries
        if record[:2] == b"\xeb\xeb":
            if record[16:] != md5.digest():
                raise ValueError("MD5 不对")
            continue
        if record[:2] != b"\xaa\x50":
            raise ValueError("格式不对")
        md5.update(record)
        kind, subtype, offset, size = struct.unpack_from("<BBII", record, 2)
        label = record[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
        entries.append((label, kind, subtype, offset, size, struct.unpack_from("<I", record, 28)[0]))
    raise ValueError("没有结尾")


def check_partition_table(table: bytes, what: str = "分区表") -> None:
    try:
        entries = partition_entries(table)
    except ValueError as error:
        raise Stop(f"{what}{error}") from None
    require([entry[:5] for entry in entries] == list(PARTITIONS) and not any(e[5] for e in entries),
            f"{what}和原厂布局不同：可能是别的硬件批次")


def check_backup(path: Path) -> bytes:
    """A whole-Flash backup of this layout; a SHA256SUMS next to it must match."""
    require(path.is_file(), f"找不到 {path}")
    data = path.read_bytes()
    require(len(data) == FLASH_SIZE, f"{path.name} 不是 16 MB 的整片备份（{len(data):,} 字节）")
    sums = path.with_name("SHA256SUMS")
    listed = listed_sha256(sums, path.name) if sums.is_file() else None
    require(listed in (None, sha256(data)), f"{path.name} 和旁边的 SHA256SUMS 对不上：备份可能坏了")
    require(data[0] == 0xE9, f"{path.name} 开头不是 Bootloader：不是整片备份")
    check_partition_table(data[PARTITION_OFFSET:PARTITION_OFFSET + SECTOR], f"{path.name} 里的分区表")
    return data


def backup_parent(value: str) -> Path:
    parent = Path(value)
    require(parent.is_dir() and os.access(parent, os.W_OK), f"备份要存进 {parent}，但那里不能写")
    return parent


def write_private(path: Path, data: bytes) -> None:
    """A new file only the user can read: a backup holds Wi-Fi passwords and activation data."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(data)


class Env:
    """Everything the commands take from outside: esptool, the latch, ports, terminal, time."""

    def __init__(self, esptool, latch, list_ports, *, out, reader=input, clock=time.monotonic,
                 sleep=time.sleep):
        self.esptool, self.latch, self.list_ports = esptool, latch, list_ports
        self.out, self.reader, self.clock, self.sleep = out, reader, clock, sleep

    def say(self, text: str) -> None:
        print(text, file=self.out, flush=True)

    def step(self, number: int, total: int, name: str, detail: str) -> None:
        self.say(f"[{number}/{total}] {name} …… {detail}")


def real_env() -> Env:
    try:
        import esptool
        import esptool.cmds
        import esptool.loader
        import serial.tools.list_ports
    except ImportError:
        raise Stop(f"没装 esptool：运行 python3 -m pip install esptool=={ESPTOOL_VERSION}") from None
    require(esptool.__version__ == ESPTOOL_VERSION,
            f"要用 esptool {ESPTOOL_VERSION}（现在是 {esptool.__version__}）："
            f"运行 python3 -m pip install esptool=={ESPTOOL_VERSION}")
    folder = str(Path(__file__).resolve().parent)
    if folder not in sys.path:
        sys.path.insert(0, folder)
    try:
        from rom_latch_gpio15 import establish_power_latch
    except ImportError:
        raise Stop("rom_latch_gpio15.py 要和本文件放在同一个文件夹（Release 里都有）") from None
    return Env(esptool, establish_power_latch, serial.tools.list_ports.comports, out=sys.stdout)


def boilon_ports(list_ports, port=None) -> list:
    """The ESP32-S3 USB-Serial/JTAG ports, or only PORT when one is given."""
    return sorted(p.device for p in list_ports()
                  if (p.vid, p.pid) == (USB_VID, USB_PID) and port in (None, p.device))


def wait_until(condition, env: Env, seconds: float = WAIT_SECONDS):
    """condition()'s first truthy result, or None after SECONDS."""
    deadline = env.clock() + seconds
    while True:
        result = condition()
        if result or env.clock() >= deadline:
            return result or None
        env.sleep(0.2)


def find_device(env: Env, port=None) -> str:
    """Have the user power the device on with the top key held, as in FLASHING.md step 1."""
    present = boilon_ports(env.list_ports, port)
    require(len(present) <= 1, f"找到 {len(present)} 个 ESP32-S3：拔掉别的，或者用 --port 指定")
    # With a card in, the firmware may start an SD update at power-on, right when we take over.
    env.say("先取出 TF 卡。")
    if present:
        env.say("设备现在开着：长按顶部键约 2 秒关机，等屏幕熄灭；USB 线别拔。")
        require(wait_until(lambda: not boilon_ports(env.list_ports, port), env),
                f"等了一分钟，{present[0]} 还在。屏幕亮着就长按顶部键关机；屏幕是黑的，就是停在了下载模式，"
                "先运行 python3 boilon_flash.py poweroff；接着别的 ESP32 板子的话，拔掉它或者用 --port 指定")
    env.say("现在按住顶部键开机，一直按着，等这里说可以松手再松开 ……")
    found = wait_until(lambda: boilon_ports(env.list_ports, port), env)
    require(found, "一分钟内没看到设备：换一根能传数据的 USB 线或者换个 USB 口，再从头来")
    require(len(found) == 1, f"找到 {len(found)} 个 ESP32-S3：拔掉别的，或者用 --port 指定")
    return found[0]


class Named(io.BytesIO):
    """An in-memory file with the name esptool prints."""

    def __init__(self, data: bytes, name: str):
        super().__init__(data)
        self.name = name


class Progress:
    """A one-line percentage for a long read or write."""

    def __init__(self, out, label: str):
        self.out, self.label, self.shown = out, label, None

    def __call__(self, done: int, total: int) -> None:
        percent = 100 * done // total if total else 100
        if percent != self.shown:
            self.shown = percent
            self.out.write(f"\r      {self.label} {percent}%" + ("\n" if percent >= 100 else ""))
            self.out.flush()


class Board:
    """One USB session: ROM with the top key held, power latch, checks, then the flasher stub."""

    def __init__(self, env: Env):
        self.env, self.esp, self.port = env, None, None
        self.latched = False  # GPIO15 holds power and no power-off was attempted yet
        self.wrote = False  # a write began: Flash may have changed
        self.log = ""  # esptool's own output of the step that failed

    @contextlib.contextmanager
    def quiet(self):
        """Keep esptool's own messages out of sight unless the step fails."""
        buffer = io.StringIO()
        try:
            with contextlib.redirect_stdout(buffer):
                yield
        except BaseException:
            self.log = buffer.getvalue()
            raise

    def connect(self, port, total: int, *, check_table: bool = True) -> None:
        env = self.env
        self.port = find_device(env, port)
        env.say(f"找到设备：{self.port}，连接中 ……")
        try:
            with self.quiet():
                self.esp = env.esptool.cmds.detect_chip(self.port, ROM_BAUD, "usb_reset")
        except Exception as error:
            raise Stop("没连上设备。多半是还没提示就松了手，或者没一直按住顶部键：等它断电，再从头来") from error
        esp = self.esp
        require(esp.CHIP_NAME == "ESP32-S3", f"连上的是 {esp.CHIP_NAME}，不是 ESP32-S3")
        require(not esp.IS_STUB, "芯片没有从头进下载模式：关机后再从头来")
        require(not esp.secure_download_mode, "芯片开着安全下载模式：和作者那台不一样")
        env.latch(esp)
        self.latched = True
        env.step(1, total, "保电", "PASS，现在可以松手了")
        # GPIO15 holds power now: wait for the user to let go before the next steps scroll by.
        while env.reader("松开顶部键了吗？[Y/n] ").strip().lower() not in ("", "y", "yes"):
            env.say("先松开顶部键，再按回车。")
        require(not esp.get_secure_boot_enabled(), "Secure Boot 开着：和作者那台不一样")
        require(not esp.get_flash_encryption_enabled(), "Flash 加密开着：和作者那台不一样")
        require(esp.get_psram_cap() == PSRAM_CAP_8MB, "芯片里不是 8 MB PSRAM：和作者那台不一样")
        with self.quiet():
            self.esp = esp.run_stub()
            self.esp.change_baud(STUB_BAUD)
        # Stop at the first error, as FLASHING.md does: esptool's own retries
        # would resend blocks, or reset and reconnect in the middle of a write.
        self.esp.WRITE_FLASH_ATTEMPTS = 1
        env.esptool.loader.WRITE_BLOCK_ATTEMPTS = 1
        with self.quiet():
            size = env.esptool.cmds.detect_flash_size(self.esp)
        require(size == "16MB", f"Flash 是 {size or '未知大小'}，不是 16 MB：和作者那台不一样")
        self.esp.flash_set_parameters(FLASH_SIZE)
        if check_table:
            check_partition_table(self.read(PARTITION_OFFSET, SECTOR))
        env.step(2, total, "检查", CHECKED + ("；分区表与原厂一致" if check_table else ""))

    def read(self, offset: int, size: int, label: str = "") -> bytes:
        progress = Progress(self.env.out, label) if label else None
        with self.quiet():
            data = self.esp.read_flash(offset, size, progress)
        require(len(data) == size, "读出来的长度不对")
        return data

    def write(self, offset: int, data: bytes, label: str) -> None:
        """Write DATA at OFFSET with esptool's compressed, MD5-checked write_flash, then read it back.

        Only that range is erased and written: any other erase or write stops it.
        """
        esp = self.esp
        size = len(data) + (-len(data) % 4)  # write_flash pads to 4 bytes
        progress = Progress(self.env.out, label)
        blocks = []
        original_begin, original_block = esp.flash_defl_begin, esp.flash_defl_block

        def begin(uncompressed, compressed, address, encrypted_write=False):
            require(not blocks and (address, uncompressed) == (offset, size) and not encrypted_write,
                    "esptool 要写的位置或大小不在计划里")
            blocks.append(original_begin(uncompressed, compressed, address, encrypted_write=encrypted_write))
            return blocks[0]

        def block(*args, **kwargs):
            result = original_block(*args, **kwargs)
            progress(args[1] + 1, blocks[0])
            return result

        def refuse(*_args, **_kwargs):
            raise Stop("esptool 要擦除或者不压缩地写：不在计划里")

        guards = dict(flash_defl_begin=begin, flash_defl_block=block, flash_begin=refuse,
                      erase_flash=refuse, erase_region=refuse)
        args = argparse.Namespace(
            chip="esp32s3", addr_filename=[(offset, Named(data, label))], erase_all=False,
            flash_mode="keep", flash_freq="keep", flash_size="keep", no_progress=True, verify=False,
            encrypt=False, encrypt_files=None, ignore_flash_encryption_efuse_setting=False,
            force=False, compress=True, no_compress=False, no_stub=False, spi_connection=None)
        self.wrote = True
        applied = []
        try:
            for name, guard in guards.items():
                setattr(esp, name, guard)
                applied.append(name)
            with self.quiet():
                self.env.esptool.cmds.write_flash(esp, args)
        finally:
            for name in applied:
                delattr(esp, name)  # back to the loader's own methods
        require(len(blocks) == 1, "esptool 没有写入")
        expected = data + b"\xff" * (padded(len(data)) - len(data))
        require(self.read(offset, len(expected), label + "：读回核对") == expected,
                f"{label}：读回来和写进去的不一样")

    def power_off(self) -> bool:
        """Controlled power-off, tried once: a watchdog reset ends the GPIO15 latch (FLASHING.md step 5)."""
        if not self.latched:
            return True
        self.latched = False
        try:
            with self.quiet():
                self.esp.watchdog_reset()
            return True
        except Exception:
            return False
        finally:
            self.close()

    def close(self) -> None:
        with contextlib.suppress(Exception):
            self.esp._port.close()

    def finish(self, number: int) -> None:
        """The last step: power off, then see the port go."""
        env = self.env
        if not self.power_off():
            env.step(number, number, "关机", POWER_OFF_FAILED)
        elif wait_until(lambda: not boilon_ports(env.list_ports, self.port), env, 5):
            env.step(number, number, "关机", "完成")
        else:
            env.step(number, number, "关机", "串口还在：屏幕亮着的话，长按顶部键关机")

    def fail(self, error: BaseException, after_write: str) -> int:
        """Say what stopped and in which state the device is; power it off if it is held on."""
        env = self.env
        if isinstance(error, KeyboardInterrupt):
            reason = "按了 Ctrl-C"
        elif isinstance(error, EOFError):
            reason = "没有读到输入"
        else:
            reason = str(error) if isinstance(error, Stop) else f"{type(error).__name__}: {error}"
        env.say(f"\n停下：{reason}")
        lines = self.log.replace("\r", "\n").split("\n")
        tail = [line for line in lines if line.strip()][-12:]
        if tail:
            env.say("esptool 的输出（最后几行）：\n  " + "\n  ".join(tail))
        if self.latched:
            env.say("设备已关机。" if self.power_off() else POWER_OFF_FAILED)
        else:
            self.close()
        env.say(after_write if self.wrote else "什么都没写。")
        return 1


def make_backup(board: Board, parent: Path, number: int, total: int) -> Path:
    """Read the whole Flash twice; save it only when both reads agree."""
    env = board.env
    env.step(number, total, "整片备份", "读两遍，每遍要几分钟")
    first = board.read(0, FLASH_SIZE, "第 1 遍")
    second = board.read(0, FLASH_SIZE, "第 2 遍")
    require(first == second, "两遍读出来不一样：换一根线或者换个 USB 口再试")
    folder = parent / time.strftime("boilon-backup-%Y%m%d-%H%M%S")
    folder.mkdir(mode=0o700)
    write_private(folder / "flash.bin", first)
    write_private(folder / "SHA256SUMS", f"{sha256(first)}  flash.bin\n".encode())
    env.say(f"      两遍一致，存进了 {folder / 'flash.bin'}。另存一份，别分享："
            "里面有 Wi-Fi 密码、激活信息和原厂固件")
    return folder


def run_install(args, env: Env) -> int:
    path = Path(args.firmware)
    firmware = check_firmware(path)
    parent = backup_parent(args.backup_dir)
    env.say(f"核对文件 …… {path.name} 与 SHA256SUMS 一致")
    board, folder = Board(env), None
    try:
        board.connect(args.port, 5)
        folder = make_backup(board, parent, 3, 5)
        env.step(4, 5, "写入", "先写 app，再写启动选择")
        board.write(OTA1_OFFSET, firmware, "app 写进 ota_1")
        board.write(OTADATA_OFFSET, OTADATA_OTA1, "启动选择指向 ota_1")
    except BaseException as error:
        return board.fail(error, f"写入没做完。备份在 {folder}，没受影响：从头再运行一次这条命令；"
                                 "还不行就用 restore 写回备份")
    board.finish(5)
    env.say(f"\n装好了。按一下顶部键开机；插上有故事的 TF 卡就能播。\n"
            "要和喵喵对话：同时长按中间和底部约 5 秒，打开配网热点 Boilon-XXXX；手机连上后打开 192.168.4.1 填 Wi-Fi。"
            "连上后屏幕显示 6 位激活码，到 xiaozhi.me 用自己的账号添加设备。\n"
            "以后升级：把新版本的 .bin 拷到 TF 卡根目录，开机就会自动装。\n"
            f"想回到原来的固件：python3 boilon_flash.py restore {folder / 'flash.bin'}")
    return 0


def run_backup(args, env: Env) -> int:
    parent = backup_parent(args.backup_dir)
    board = Board(env)
    try:
        board.connect(args.port, 4)
        folder = make_backup(board, parent, 3, 4)
    except BaseException as error:
        return board.fail(error, "")
    board.finish(4)
    env.say(f"\n备份好了：{folder / 'flash.bin'}，SHA-256 记在同一个文件夹的 SHA256SUMS 里。")
    return 0


def run_restore(args, env: Env) -> int:
    path = Path(args.backup)
    image = check_backup(path)
    env.say(f"核对备份 …… {path.name}，16 MB，分区表与原厂一致")
    env.say("这会把整片 Flash 写回备份那一刻：之后装的固件、Wi-Fi、设置和播放进度都回到那时。")
    if env.reader("确认就输入 yes：").strip().lower() != "yes":
        env.say("没有确认，什么都没做。")
        return 0
    board = Board(env)
    try:
        board.connect(args.port, 4, check_table=False)
        env.step(3, 4, "写回", "整片 16 MB，写完读回核对")
        board.write(0, image, "写回备份")
    except BaseException as error:
        return board.fail(error, "写回没做完，设备可能开不了机：从头再运行一次同样的 restore 命令"
                                 "（下载模式在芯片里，写坏了也能进）")
    board.finish(4)
    env.say("\n写回好了，和备份一致。按一下顶部键开机，设备回到备份那一刻。"
            "备份里是原厂固件的话，它联网后可能自己升级，介意就先断网再开机。")
    return 0


def run_poweroff(args, env: Env) -> int:
    ports = boilon_ports(env.list_ports, args.port)
    require(ports, "没看到设备的串口：它已经关机了")
    require(len(ports) == 1, f"找到 {len(ports)} 个 ESP32-S3：拔掉别的，或者用 --port 指定")
    board = Board(env)
    board.port = ports[0]
    try:
        with board.quiet():
            board.esp = env.esptool.cmds.detect_chip(board.port, ROM_BAUD, "no_reset")
    except Exception as error:
        raise Stop("连不上：设备不在下载模式。屏幕亮着的话，长按顶部键关机") from error
    board.latched = True  # whatever holds power now, the watchdog reset ends it
    board.finish(1)
    return 0


def parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", help="设备的串口；默认自动找 ESP32-S3 的 USB 串口")
    result = argparse.ArgumentParser(prog="boilon_flash.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = result.add_subparsers(dest="command", required=True)
    install = commands.add_parser("install", parents=[common], help="整片备份，再写入固件")
    install.add_argument("firmware", help="Release 里的 boilon-ai-player-vX.Y.Z.bin，旁边要有 SHA256SUMS")
    install.add_argument("--backup-dir", default=".", help="备份文件夹建在哪里，默认是当前文件夹")
    backup = commands.add_parser("backup", parents=[common], help="只做整片备份")
    backup.add_argument("--backup-dir", default=".", help="备份文件夹建在哪里，默认是当前文件夹")
    restore = commands.add_parser("restore", parents=[common], help="把整片备份写回去")
    restore.add_argument("backup", help="备份文件夹里的 flash.bin")
    commands.add_parser("poweroff", parents=[common], help="让停在下载模式的设备关机")
    return result


COMMANDS = {"install": run_install, "backup": run_backup, "restore": run_restore,
            "poweroff": run_poweroff}


def main(argv=None, env: Env | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        env = env or real_env()
        return COMMANDS[args.command](args, env)
    except (Stop, EOFError, KeyboardInterrupt) as error:
        reason = error if isinstance(error, Stop) else "没有输入，或者按了 Ctrl-C"
        print(f"停下：{reason}", file=env.out if env else sys.stdout, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
