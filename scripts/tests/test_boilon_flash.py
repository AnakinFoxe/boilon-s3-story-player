import hashlib
import importlib.util
import io
import struct
import sys
import tempfile
import types
import unittest
import zlib
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/boilon/boilon_flash.py"
OTADATA_SCRIPT = ROOT / "scripts/boilon/make_otadata.py"
PARTITION_CSV = ROOT / "partitions/boilon-s3-gen2.csv"
# make_otadata.py's image, as test_boilon_otadata.py pins it.
OTADATA_SHA256 = "a1ed149677f974e7b1fd3b926c817a1db72ce01ca26b31929937d4ab01f5be22"
PORT = "/dev/cu.usbmodem1101"
SECTOR = 0x1000


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


flash = load(SCRIPT, "boilon_flash")
FLASH_SIZE = flash.FLASH_SIZE


def partition_table(entries=flash.PARTITIONS, md5=True):
    records = b""
    for label, kind, subtype, offset, size in entries:
        records += (b"\xaa\x50" + struct.pack("<BBII", kind, subtype, offset, size)
                    + label.encode().ljust(16, b"\0") + b"\0" * 4)
    if md5:
        records += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(records).digest()
    return records + b"\xff" * (SECTOR - len(records))


def device_image(table=None):
    """16 MiB laid out like the device: bootloader, table, NVS, boot selection, apps, assets."""
    image = bytearray(b"\xff" * FLASH_SIZE)
    image[0:4] = b"\xe9\x03\x02\x4f"
    image[0x8000:0x9000] = table or partition_table()
    image[0x9000:0x9010] = b"nvs-wifi-and-ids"
    image[0x11000:0x12000] = flash.otadata_sector(1, 2)
    image[0x20000:0x20004] = b"\xe9\x06\x02\x4f"
    image[0x400000:0x400004] = b"\xe9\x06\x02\x4f"
    image[0x800000:0x800006] = b"assets"
    return image


def app_image(version="1.4.0", size=0x3003):
    data = bytearray(size)
    data[0] = 0xE9
    data[12:14] = (9).to_bytes(2, "little")
    data[32:36] = (0xABCD5432).to_bytes(4, "little")
    name = f"boilon-ai-player v{version}".encode()
    data[0x100:0x100 + len(name)] = name
    return bytes(data)


def release(folder, data=None, name="boilon-ai-player-v1.4.0.bin", sums=True):
    path = Path(folder) / name
    data = app_image() if data is None else data
    path.write_bytes(data)
    if sums:
        (Path(folder) / "SHA256SUMS").write_text(
            f"{hashlib.sha256(data).hexdigest()}  {name}\n"
            f"{'0' * 64}  boilon-otadata-ota1.bin\n", encoding="utf-8")
    return path


class FakeDevice:
    """A device's Flash and eFuses, shared by its ROM and stub loaders; events in order."""

    def __init__(self, image=None, **options):
        self.flash = bytearray(device_image() if image is None else image)
        self.events = []
        self.on = False
        self.chip = options.get("chip", "ESP32-S3")
        self.secure_download = options.get("secure_download", False)
        self.secure_boot = options.get("secure_boot", False)
        self.encrypted = options.get("encrypted", False)
        self.psram = options.get("psram", 1)
        self.flash_size = options.get("flash_size", "16MB")
        self.connect_error = options.get("connect_error", False)
        self.read_faults = options.get("read_faults", {})  # (offset, length) -> number of the read to corrupt
        self.interrupt_read = options.get("interrupt_read")
        self.reads = {}


class FakeLoader:
    FLASH_WRITE_SIZE = 0x4000
    WRITE_FLASH_ATTEMPTS = 2

    def __init__(self, device, stub=False):
        self.device = device
        self.CHIP_NAME = device.chip
        self.IS_STUB = stub
        self.secure_download_mode = device.secure_download
        self._port = types.SimpleNamespace(close=lambda: device.events.append(("close",)))
        self.pending = None

    def get_secure_boot_enabled(self):
        self.device.events.append(("security",))
        return self.device.secure_boot

    def get_flash_encryption_enabled(self):
        return self.device.encrypted

    def get_psram_cap(self):
        return self.device.psram

    def run_stub(self):
        self.device.events.append(("run_stub",))
        return FakeLoader(self.device, stub=True)

    def change_baud(self, baud):
        self.device.events.append(("baud", baud))

    def flash_set_parameters(self, size):
        self.device.events.append(("flash_size", size))

    def read_flash(self, offset, length, progress_fn=None):
        device = self.device
        device.events.append(("read", offset, length))
        if device.interrupt_read == (offset, length):
            raise KeyboardInterrupt
        count = device.reads[(offset, length)] = device.reads.get((offset, length), 0) + 1
        data = bytes(device.flash[offset:offset + length])
        if device.read_faults.get((offset, length)) == count:
            data = bytes([data[0] ^ 1]) + data[1:]
        if progress_fn:
            progress_fn(length, length)
            progress_fn(length, length)
        return data

    def flash_defl_begin(self, size, compsize, offset, encrypted_write=False):
        self.device.events.append(("begin", offset, size))
        end = -(-(offset + size) // SECTOR) * SECTOR
        self.device.flash[offset:end] = b"\xff" * (end - offset)
        self.pending = [offset, compsize, b""]
        return -(-compsize // self.FLASH_WRITE_SIZE)

    def flash_defl_block(self, data, seq, timeout=3):
        self.pending[2] += data
        offset, compsize, received = self.pending
        if len(received) == compsize:
            image = zlib.decompress(received)
            self.device.flash[offset:offset + len(image)] = image

    def flash_defl_finish(self, reboot=False, timeout=3):
        self.device.events.append(("finish",))

    def flash_md5sum(self, address, size):
        return hashlib.md5(self.device.flash[address:address + size]).hexdigest()

    def flash_begin(self, *args, **kwargs):
        raise AssertionError("uncompressed write")

    def erase_flash(self):
        self.device.flash[:] = b"\xff" * FLASH_SIZE

    def erase_region(self, offset, size):
        self.device.flash[offset:offset + size] = b"\xff" * size

    def watchdog_reset(self):
        self.device.events.append(("watchdog",))
        self.device.on = False


def fake_esptool(device):
    def detect_chip(port, baud, connect_mode):
        device.events.append(("detect", port, baud, connect_mode))
        if device.connect_error:
            raise RuntimeError("Failed to connect to ESP32-S3: No serial data received.")
        print("Connecting....")
        return FakeLoader(device)

    def detect_flash_size(esp):
        return device.flash_size

    def write_flash(esp, args):
        """esptool 4.12.0's write_flash calls, in order, for the arguments boilon_flash.py passes."""
        assert args.compress and not (args.erase_all or args.encrypt or args.encrypt_files or args.no_stub)
        assert (args.flash_mode, args.flash_freq, args.flash_size) == ("keep",) * 3
        for address, source in args.addr_filename:
            image = source.read()
            image += b"\xff" * (-len(image) % 4)
            packed = zlib.compress(image, 9)
            blocks = esp.flash_defl_begin(len(image), len(packed), address, encrypted_write=False)
            for seq in range(blocks):
                size = esp.FLASH_WRITE_SIZE
                esp.flash_defl_block(packed[seq * size:(seq + 1) * size], seq, timeout=3)
            esp.flash_defl_finish(reboot=False, timeout=3)
            if esp.flash_md5sum(address, len(image)) != hashlib.md5(image).hexdigest():
                raise RuntimeError("MD5 of file does not match data in flash!")

    cmds = types.SimpleNamespace(detect_chip=detect_chip, detect_flash_size=detect_flash_size,
                                 write_flash=write_flash)
    return types.SimpleNamespace(__version__="4.12.0", cmds=cmds,
                                 loader=types.SimpleNamespace(WRITE_BLOCK_ATTEMPTS=3))


class Ports:
    """USB ports as polled: the scripted presence first (the user powering off or holding
    the top key), then whatever the device's power says."""

    def __init__(self, device, presence=(False, False, True), extra=()):
        self.device, self.presence, self.extra = device, list(presence), list(extra)

    def __call__(self):
        if self.presence:
            self.device.on = self.presence.pop(0)
        found = [types.SimpleNamespace(device=PORT, vid=0x303A, pid=0x1001)] if self.device.on else []
        return found + [types.SimpleNamespace(device=name, vid=vid, pid=pid) for name, vid, pid in self.extra]


class Clock:
    def __init__(self):
        self.now = 0.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def make_env(device, ports=None, answer="yes"):
    """ANSWER is typed at every prompt; a list is typed in turn, where an exception class is raised."""
    out, clock = io.StringIO(), Clock()
    answers = list(answer) if isinstance(answer, list) else None

    def latch(esp):
        assert not esp.IS_STUB
        device.events.append(("latch",))

    def reader(prompt):
        device.events.append(("ask", prompt))
        value = answers.pop(0) if answers is not None else answer
        if isinstance(value, type):
            raise value
        return value

    env = flash.Env(fake_esptool(device), latch, ports or Ports(device), out=out,
                    reader=reader, clock=clock.time, sleep=clock.sleep)
    return env, out


def kinds(device):
    return [event[0] for event in device.events]


class FileCheckTests(unittest.TestCase):
    def test_boot_selection_is_make_otadata_image(self):
        otadata = load(OTADATA_SCRIPT, "boilon_make_otadata")
        self.assertEqual(flash.OTADATA_OTA1, otadata.OTA_1)
        self.assertEqual(hashlib.sha256(flash.OTADATA_OTA1).hexdigest(), OTADATA_SHA256)

    def test_layout_is_the_partition_csv(self):
        rows = [line.split("#", 1)[0].split(",") for line in PARTITION_CSV.read_text().splitlines()]
        types_ = {"app": 0x00, "data": 0x01}
        subtypes = {"nvs": 0x02, "ota": 0x00, "phy": 0x01, "ota_0": 0x10, "ota_1": 0x11, "spiffs": 0x82}
        layout = tuple((r[0].strip(), types_[r[1].strip()], subtypes[r[2].strip()], int(r[3], 16), int(r[4], 16))
                       for r in rows if len(r) > 4)
        self.assertEqual(flash.PARTITIONS, layout)

    def test_release_file_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = release(folder)
            self.assertEqual(flash.check_firmware(path), path.read_bytes())

    def test_release_file_refusals(self):
        cases = [
            ({"name": "firmware.bin"}, "原名"),
            ({"sums": False}, "没有 SHA256SUMS"),
            ({"data": app_image(version="1.3.0")}, "版本名"),
            ({"data": b"\x00" + app_image()[1:]}, "不是 ESP32-S3"),
            ({"data": app_image()[:12] + b"\x05\x00" + app_image()[14:]}, "不是 ESP32-S3"),
            ({"data": app_image()[:32] + b"\0\0\0\0" + app_image()[36:]}, "不是 ESP32-S3"),
            ({"data": app_image(size=0x3E0001)}, "放不进 ota_1"),
        ]
        for options, message in cases:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as folder:
                path = release(folder, **options)
                with self.assertRaisesRegex(flash.Stop, message):
                    flash.check_firmware(path)

    def test_sums_must_list_and_match_the_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = release(folder)
            sums = Path(folder) / "SHA256SUMS"
            sums.write_text(f"{'1' * 64}  {path.name}\n", encoding="utf-8")
            with self.assertRaisesRegex(flash.Stop, "对不上"):
                flash.check_firmware(path)
            sums.write_text(f"{'1' * 64}  boilon-ai-player-v1.3.0.bin\n", encoding="utf-8")
            with self.assertRaisesRegex(flash.Stop, "没有"):
                flash.check_firmware(path)

    def test_partition_table(self):
        flash.check_partition_table(partition_table())
        flash.check_partition_table(partition_table(md5=False))
        moved = [list(entry) for entry in flash.PARTITIONS]
        moved[4][3] = 0x410000
        bad_md5 = bytearray(partition_table())
        bad_md5[0xD0] ^= 1
        encrypted = bytearray(partition_table(md5=False))
        encrypted[28] = 1
        for table, message in ((partition_table([tuple(e) for e in moved]), "原厂布局"),
                               (partition_table(flash.PARTITIONS[:5]), "原厂布局"),
                               (bytes(bad_md5), "MD5"), (bytes(encrypted), "原厂布局"),
                               (b"\x00" * SECTOR, "格式"), (partition_table()[:0x20] * 128, "结尾")):
            with self.subTest(message=message), self.assertRaisesRegex(flash.Stop, message):
                flash.check_partition_table(table)

    def test_backup_refusals(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "flash.bin"
            image = device_image()
            path.write_bytes(image[:-SECTOR])
            with self.assertRaisesRegex(flash.Stop, "16 MB"):
                flash.check_backup(path)
            path.write_bytes(image)
            (Path(folder) / "SHA256SUMS").write_text(f"{'2' * 64}  flash.bin\n", encoding="utf-8")
            with self.assertRaisesRegex(flash.Stop, "对不上"):
                flash.check_backup(path)
            (Path(folder) / "SHA256SUMS").unlink()
            path.write_bytes(device_image(partition_table(flash.PARTITIONS[:5])))
            with self.assertRaisesRegex(flash.Stop, "原厂布局"):
                flash.check_backup(path)
            path.write_bytes(image)
            self.assertEqual(flash.check_backup(path), bytes(image))


class InstallTests(unittest.TestCase):
    def run_install(self, device, ports=None, firmware=None, port=None):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        path = release(self.folder.name, firmware)
        env, out = make_env(device, ports)
        argv = ["install", str(path), "--backup-dir", self.folder.name] + (["--port", port] if port else [])
        code = flash.main(argv, env)
        return code, out.getvalue(), env

    def backups(self):
        return sorted(Path(self.folder.name).glob("boilon-backup-*/flash.bin"))

    def test_backs_up_twice_then_writes_only_ota1_then_otadata(self):
        device = FakeDevice()
        before = bytes(device.flash)
        app = app_image()
        code, out, env = self.run_install(device, firmware=app)
        self.assertEqual(code, 0, out)
        after = bytes(device.flash)
        self.assertEqual(after[0x400000:0x404000], app + b"\xff" * (0x4000 - len(app)))
        self.assertEqual(after[0x11000:0x13000], flash.OTADATA_OTA1)
        self.assertEqual(after[:0x11000], before[:0x11000])
        self.assertEqual(after[0x13000:0x400000], before[0x13000:0x400000])
        self.assertEqual(after[0x404000:], before[0x404000:])
        [backup] = self.backups()
        self.assertEqual(backup.read_bytes(), before)
        self.assertEqual((backup.parent / "SHA256SUMS").read_text(),
                         f"{hashlib.sha256(before).hexdigest()}  flash.bin\n")
        events = device.events
        self.assertEqual(events[0], ("detect", PORT, 115200, "usb_reset"))
        begins = [event for event in events if event[0] == "begin"]
        self.assertEqual(begins, [("begin", 0x400000, 0x3004), ("begin", 0x11000, 0x2000)])
        full = [index for index, event in enumerate(events) if event == ("read", 0, FLASH_SIZE)]
        self.assertEqual(len(full), 2)
        self.assertLess(full[1], events.index(begins[0]))
        order = kinds(device)
        self.assertLess(order.index("latch"), order.index("ask"))
        self.assertLess(order.index("ask"), order.index("security"))
        self.assertLess(order.index("security"), order.index("run_stub"))
        self.assertEqual(order.count("ask"), 1)
        self.assertIn(("read", 0x400000, 0x4000), events[events.index(begins[0]):events.index(begins[1])])
        self.assertEqual(order[-2:], ["watchdog", "close"])
        self.assertEqual(env.esptool.loader.WRITE_BLOCK_ATTEMPTS, 1)
        self.assertIn("[1/5] 保电 …… PASS，现在可以松手了", out)
        self.assertIn("[5/5] 关机 …… 完成", out)
        self.assertIn("装好了", out)

    def test_no_write_when_the_two_reads_differ(self):
        device = FakeDevice(read_faults={(0, FLASH_SIZE): 2})
        code, out, _ = self.run_install(device)
        self.assertEqual(code, 1)
        self.assertNotIn("begin", kinds(device))
        self.assertIn("watchdog", kinds(device))
        self.assertEqual(self.backups(), [])
        self.assertIn("两遍读出来不一样", out)
        self.assertIn("设备已关机。", out)
        self.assertIn("什么都没写。", out)

    def test_security_flash_and_layout_stop_before_any_read(self):
        cases = [({"secure_boot": True}, "Secure Boot"), ({"encrypted": True}, "Flash 加密"),
                 ({"psram": 0}, "PSRAM"), ({"flash_size": "8MB"}, "不是 16 MB"),
                 ({"image": device_image(partition_table(flash.PARTITIONS[1:]))}, "原厂布局")]
        for options, message in cases:
            with self.subTest(message=message):
                device = FakeDevice(**options)
                before = bytes(device.flash)
                code, out, _ = self.run_install(device)
                self.assertEqual(code, 1)
                self.assertIn(message, out)
                self.assertNotIn(("read", 0, FLASH_SIZE), device.events)
                self.assertNotIn("begin", kinds(device))
                self.assertIn("watchdog", kinds(device))
                self.assertEqual(bytes(device.flash), before)

    def test_wrong_chip_or_lost_connection_stops_before_the_latch(self):
        for options, message in (({"chip": "ESP32-C3"}, "ESP32-C3"), ({"connect_error": True}, "没连上设备"),
                                 ({"secure_download": True}, "安全下载模式")):
            with self.subTest(message=message):
                device = FakeDevice(**options)
                code, out, _ = self.run_install(device)
                self.assertEqual(code, 1)
                self.assertIn(message, out)
                self.assertNotIn("latch", kinds(device))
                self.assertNotIn("watchdog", kinds(device))
                self.assertIn("什么都没写。", out)

    def test_failed_readback_names_the_backup(self):
        device = FakeDevice(read_faults={(0x400000, 0x4000): 1})
        code, out, _ = self.run_install(device)
        self.assertEqual(code, 1)
        [backup] = self.backups()
        self.assertIn("读回来和写进去的不一样", out)
        self.assertIn(f"备份在 {backup.parent}", out)
        self.assertNotIn(("begin", 0x11000, 0x2000), device.events)
        self.assertIn("watchdog", kinds(device))

    def test_waits_until_the_top_key_is_released(self):
        device = FakeDevice()
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        env, out = make_env(device, answer=["n", ""])
        argv = ["install", str(release(self.folder.name)), "--backup-dir", self.folder.name]
        self.assertEqual(flash.main(argv, env), 0, out.getvalue())
        asks = [event for event in device.events if event[0] == "ask"]
        self.assertEqual(asks, [("ask", "松开顶部键了吗？[Y/n] ")] * 2)
        order = kinds(device)
        self.assertLess(order.index("latch"), order.index("ask"))
        last_ask = len(order) - 1 - order[::-1].index("ask")
        self.assertLess(last_ask, order.index("security"))
        self.assertEqual(out.getvalue().count("先松开顶部键，再按回车。"), 1)

    def test_no_answer_at_release_powers_off(self):
        device = FakeDevice()
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        env, out = make_env(device, answer=[EOFError])
        argv = ["install", str(release(self.folder.name)), "--backup-dir", self.folder.name]
        self.assertEqual(flash.main(argv, env), 1)
        self.assertIn("停下：没有读到输入", out.getvalue())
        self.assertNotIn("security", kinds(device))
        self.assertIn("watchdog", kinds(device))
        self.assertIn("什么都没写。", out.getvalue())

    def test_ctrl_c_during_the_backup_powers_off(self):
        device = FakeDevice(interrupt_read=(0, FLASH_SIZE))
        code, out, _ = self.run_install(device)
        self.assertEqual(code, 1)
        self.assertIn("按了 Ctrl-C", out)
        self.assertIn("watchdog", kinds(device))
        self.assertIn("什么都没写。", out)

    def test_device_on_at_start_must_power_off_first(self):
        device = FakeDevice()
        code, out, _ = self.run_install(device, Ports(device, [True, True, False, False, True]))
        self.assertEqual(code, 0, out)
        self.assertIn("设备现在开着", out)
        self.assertLess(out.index("设备现在开着"), out.index("现在按住顶部键开机"))

    def test_device_never_seen(self):
        device = FakeDevice()
        code, out, _ = self.run_install(device, Ports(device, []))
        self.assertEqual(code, 1)
        self.assertIn("一分钟内没看到设备", out)
        self.assertEqual(device.events, [])

    def test_bad_file_stops_before_the_device(self):
        device = FakeDevice()
        code, out, _ = self.run_install(device, firmware=app_image(version="9.9.9"))
        self.assertEqual(code, 1)
        self.assertIn("停下：", out)
        self.assertEqual(device.events, [])

    def test_two_espressif_ports_need_port(self):
        other = [("/dev/cu.usbmodem2101", 0x303A, 0x1001)]
        device = FakeDevice()
        code, out, _ = self.run_install(device, Ports(device, [True], extra=other))
        self.assertEqual(code, 1)
        self.assertIn("找到 2 个 ESP32-S3", out)
        self.assertEqual(device.events, [])
        device = FakeDevice()
        code, out, _ = self.run_install(device, Ports(device, extra=other), port=PORT)
        self.assertEqual(code, 0, out)
        self.assertEqual(device.events[0], ("detect", PORT, 115200, "usb_reset"))


class WriteGuardTests(unittest.TestCase):
    def board(self, write_flash):
        device = FakeDevice()
        env, _ = make_env(device)
        env.esptool.cmds.write_flash = write_flash
        board = flash.Board(env)
        board.esp = FakeLoader(device, stub=True)
        return device, board

    def test_other_range_is_refused(self):
        def write_flash(esp, args):
            esp.flash_defl_begin(0x2000, 0x100, 0x9000, encrypted_write=False)

        device, board = self.board(write_flash)
        with self.assertRaisesRegex(flash.Stop, "不在计划里"):
            board.write(flash.OTADATA_OFFSET, flash.OTADATA_OTA1, "启动选择")
        self.assertNotIn("begin", kinds(device))
        self.assertNotIn("flash_defl_begin", vars(board.esp))

    def test_erase_is_refused(self):
        def write_flash(esp, args):
            esp.erase_flash()

        device, board = self.board(write_flash)
        before = bytes(device.flash)
        with self.assertRaisesRegex(flash.Stop, "擦除"):
            board.write(flash.OTADATA_OFFSET, flash.OTADATA_OTA1, "启动选择")
        self.assertEqual(bytes(device.flash), before)
        self.assertNotIn("erase_flash", vars(board.esp))


class RestoreAndPowerOffTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.image = device_image()
        self.image[0x400000:0x400010] = b"factory-firmware"
        self.backup = Path(self.folder.name) / "flash.bin"
        self.backup.write_bytes(self.image)
        (Path(self.folder.name) / "SHA256SUMS").write_text(
            f"{hashlib.sha256(self.image).hexdigest()}  flash.bin\n", encoding="utf-8")

    def test_restore_writes_the_whole_backup_back(self):
        device = FakeDevice(device_image(partition_table(flash.PARTITIONS[:5])))
        env, out = make_env(device)
        self.assertEqual(flash.main(["restore", str(self.backup)], env), 0, out.getvalue())
        self.assertEqual(bytes(device.flash), bytes(self.image))
        self.assertEqual([event for event in device.events if event[0] == "begin"], [("begin", 0, FLASH_SIZE)])
        self.assertIn(("read", 0, FLASH_SIZE), device.events)
        self.assertIn("watchdog", kinds(device))

    def test_restore_needs_yes(self):
        device = FakeDevice()
        env, out = make_env(device, answer=["no", EOFError])
        self.assertEqual(flash.main(["restore", str(self.backup)], env), 0)
        self.assertIn("什么都没做", out.getvalue())
        self.assertEqual(flash.main(["restore", str(self.backup)], env), 1)
        self.assertIn("停下：没有输入", out.getvalue())
        self.assertEqual(kinds(device), ["ask", "ask"])

    def test_failed_restore_says_run_it_again(self):
        device = FakeDevice(read_faults={(0, FLASH_SIZE): 1})
        env, out = make_env(device)
        self.assertEqual(flash.main(["restore", str(self.backup)], env), 1)
        self.assertIn("从头再运行一次同样的 restore 命令", out.getvalue())

    def test_poweroff(self):
        device = FakeDevice()
        device.on = True
        env, out = make_env(device, Ports(device, []))
        self.assertEqual(flash.main(["poweroff"], env), 0)
        self.assertEqual(device.events[0], ("detect", PORT, 115200, "no_reset"))
        self.assertIn("watchdog", kinds(device))
        self.assertIn("[1/1] 关机 …… 完成", out.getvalue())
        env, out = make_env(device, Ports(device, []))
        self.assertEqual(flash.main(["poweroff"], env), 1)
        self.assertIn("已经关机了", out.getvalue())


class EnvironmentTests(unittest.TestCase):
    def modules(self, version):
        esptool = types.ModuleType("esptool")
        esptool.__version__ = version
        esptool.cmds = types.ModuleType("esptool.cmds")
        esptool.cmds.detect_chip = object
        esptool.loader = types.ModuleType("esptool.loader")
        serial = types.ModuleType("serial")
        serial.tools = types.ModuleType("serial.tools")
        serial.tools.list_ports = types.ModuleType("serial.tools.list_ports")
        serial.tools.list_ports.comports = list
        return {"esptool": esptool, "esptool.cmds": esptool.cmds, "esptool.loader": esptool.loader,
                "serial": serial, "serial.tools": serial.tools,
                "serial.tools.list_ports": serial.tools.list_ports}

    def test_needs_esptool_4_12_0_and_the_latch_script(self):
        with mock.patch.dict(sys.modules, self.modules("5.0.2")):
            with self.assertRaisesRegex(flash.Stop, "4.12.0"):
                flash.real_env()
        with mock.patch.dict(sys.modules, self.modules("4.12.0")), mock.patch.object(sys, "path", list(sys.path)):
            sys.modules.pop("rom_latch_gpio15", None)
            env = flash.real_env()
            self.assertEqual(env.latch.__module__, "rom_latch_gpio15")
            self.assertIs(env.list_ports, list)


if __name__ == "__main__":
    unittest.main()
