import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"


class BoilonP15ReadOnlyTests(unittest.TestCase):
    def test_actual_adapter_against_native_card_fake(self):
        configured = os.environ.get("CXX")
        compiler = shlex.split(configured) if configured else [shutil.which("c++")]
        self.assertTrue(compiler and compiler[0], "A host C++ compiler is required")
        headers = {
            "ff.h": r'''
#pragma once
#include <cstdint>
using BYTE = unsigned char;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
''',
            "diskio_impl.h": r'''
#pragma once
#include "ff.h"
using DSTATUS = BYTE;
enum DRESULT { RES_OK, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR };
constexpr BYTE STA_NOINIT = 1, STA_PROTECT = 4, FF_DRV_NOT_USED = 0xff;
constexpr BYTE CTRL_SYNC = 0, GET_SECTOR_COUNT = 1, GET_SECTOR_SIZE = 2;
constexpr BYTE GET_BLOCK_SIZE = 3, CTRL_TRIM = 4;
struct ff_diskio_impl_t {
    DSTATUS (*init)(BYTE);
    DSTATUS (*status)(BYTE);
    DRESULT (*read)(BYTE, BYTE*, DWORD, UINT);
    DRESULT (*write)(BYTE, const BYTE*, DWORD, UINT);
    DRESULT (*ioctl)(BYTE, BYTE, void*);
};
''',
            "sdmmc_cmd.h": r'''
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
struct sdmmc_card_t {
    struct { uint32_t capacity; uint32_t sector_size; } csd;
};
esp_err_t sdmmc_read_sectors(sdmmc_card_t*, void*, size_t, size_t);
''',
        }
        source = r'''
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <limits>
#include "p1_5_sd_readonly.h"

static unsigned reads = 0;
static esp_err_t next_read = ESP_OK;
static sdmmc_card_t* observed_card = nullptr;
static size_t observed_sector = 0, observed_count = 0;

esp_err_t sdmmc_read_sectors(sdmmc_card_t* card, void* buffer,
                            size_t sector, size_t count) {
    ++reads;
    observed_card = card;
    observed_sector = sector;
    observed_count = count;
    if (next_read == ESP_OK)
        std::memset(buffer, 0xa5, count * card->csd.sector_size);
    return next_read;
}

int main() {
    namespace probe = boilon::sd_probe;
    const auto& io = probe::kReadOnly;
    constexpr BYTE drive = 2;
    sdmmc_card_t fixture{{16, 512}};
    BYTE buffer[1024] = {};
    DWORD sectors = 0;
    WORD sector_size = 0;
    DWORD trim_range[2] = {0, 15};

    // No card and a mismatched drive must never reach the underlying reader.
    assert(io.init(drive) == (STA_NOINIT | STA_PROTECT));
    assert(io.status(drive) == (STA_NOINIT | STA_PROTECT));
    assert(io.read(drive, buffer, 0, 1) == RES_NOTRDY);
    assert(io.ioctl(drive, GET_SECTOR_COUNT, &sectors) == RES_NOTRDY);
    probe::card = &fixture;
    probe::drive = drive;
    assert(io.init(drive) == STA_PROTECT);
    assert(io.status(drive) == STA_PROTECT);
    assert(io.status(drive + 1) == (STA_NOINIT | STA_PROTECT));
    assert(io.read(drive + 1, buffer, 0, 1) == RES_NOTRDY);
    assert(io.ioctl(drive + 1, GET_SECTOR_COUNT, &sectors) == RES_NOTRDY);
    assert(reads == 0);

    // Writes are refused regardless of drive, payload, or requested range.
    assert(io.write(drive, buffer, 0, 1) == RES_WRPRT);
    assert(io.write(drive + 1, nullptr, 0, 0) == RES_WRPRT);
    assert(io.write(drive, buffer, UINT32_MAX, UINT32_MAX) == RES_WRPRT);
    assert(io.ioctl(drive, CTRL_TRIM, trim_range) == RES_PARERR);
    assert(io.ioctl(drive, GET_BLOCK_SIZE, &sectors) == RES_PARERR);
    assert(io.ioctl(drive, 255, &sectors) == RES_PARERR);
    assert(io.ioctl(drive, CTRL_TRIM, nullptr) == RES_PARERR);
    assert(io.ioctl(drive, CTRL_SYNC, nullptr) == RES_OK);
    assert(io.ioctl(drive, GET_SECTOR_COUNT, &sectors) == RES_OK);
    assert(io.ioctl(drive, GET_SECTOR_SIZE, &sector_size) == RES_OK);
    assert(sectors == 16 && sector_size == 512);
    assert(io.ioctl(drive, GET_SECTOR_COUNT, nullptr) == RES_PARERR);
    assert(io.ioctl(drive, GET_SECTOR_SIZE, nullptr) == RES_PARERR);
    assert(reads == 0 && fixture.csd.capacity == 16 && fixture.csd.sector_size == 512);

    // Rejected arguments neither issue card reads nor poison later valid reads.
    assert(io.read(drive, nullptr, 0, 1) == RES_PARERR);
    assert(io.read(drive, buffer, 0, 0) == RES_PARERR);
    assert(io.read(drive, buffer, 16, 1) == RES_PARERR);
    assert(io.read(drive, buffer, 15, 2) == RES_PARERR);
    assert(io.read(drive, buffer, 1, UINT32_MAX) == RES_PARERR);
    assert(io.read(drive, buffer, UINT32_MAX, 2) == RES_PARERR);
    assert(reads == 0 && !probe::read_failed);
    assert(io.read(drive, buffer, 14, 2) == RES_OK);
    assert(reads == 1 && observed_card == &fixture);
    assert(observed_sector == 14 && observed_count == 2);
    for (BYTE byte : buffer)
        assert(byte == 0xa5);
    assert(io.read(drive, buffer, 15, 1) == RES_OK);
    assert(reads == 2 && observed_sector == 15 && observed_count == 1);

    // The first card error is terminal even if the fake card recovers afterward.
    next_read = ESP_FAIL;
    assert(io.read(drive, buffer, 0, 1) == RES_ERROR);
    assert(reads == 3 && probe::read_failed);
    next_read = ESP_OK;
    assert(io.init(drive) == (STA_NOINIT | STA_PROTECT));
    assert(io.status(drive) == (STA_NOINIT | STA_PROTECT));
    assert(io.read(drive, buffer, 0, 1) == RES_NOTRDY);
    assert(io.read(drive, buffer, 1, 1) == RES_NOTRDY);
    assert(io.ioctl(drive, GET_SECTOR_COUNT, &sectors) == RES_NOTRDY);
    assert(io.ioctl(drive, CTRL_SYNC, nullptr) == RES_NOTRDY);
    assert(io.write(drive, buffer, 0, 1) == RES_WRPRT);
    assert(reads == 3);
}
'''
        with tempfile.TemporaryDirectory(prefix="boilon-p1-5-readonly-") as directory:
            temp = Path(directory)
            for name, contents in headers.items():
                (temp / name).write_text(contents, encoding="utf-8")
            executable = temp / "readonly-test"
            compiled = subprocess.run(
                compiler + [
                    "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-fno-exceptions", "-fno-rtti", "-I", str(temp),
                    "-I", str(BOARD), "-x", "c++", "-", "-o", str(executable),
                ],
                input=source, capture_output=True, text=True, timeout=30, cwd=ROOT,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            checked = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=10, cwd=ROOT,
            )
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
