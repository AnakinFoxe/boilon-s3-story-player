#pragma once

#include <diskio_impl.h>
#include <ff.h>
#include <sdmmc_cmd.h>

// Board-local, single-card diagnostic adapter. Register BEFORE the first mount;
// never use the default SD diskio (which has write and erase call edges).
namespace boilon::sd_probe {
inline sdmmc_card_t* card = nullptr;
inline BYTE drive = FF_DRV_NOT_USED;
inline bool read_failed = false;

inline DSTATUS Status(BYTE requested) {
    return STA_PROTECT | ((!card || read_failed || requested != drive) ? STA_NOINIT : 0);
}

inline DRESULT Read(BYTE requested, BYTE* buffer, DWORD sector, UINT count) {
    if (Status(requested) & STA_NOINIT)
        return RES_NOTRDY;
    if (!buffer || !count || sector >= card->csd.capacity || count > card->csd.capacity - sector)
        return RES_PARERR;
    if (sdmmc_read_sectors(card, buffer, sector, count) != ESP_OK) {
        read_failed = true;  // No later filesystem call may retry this failed read.
        return RES_ERROR;
    }
    return RES_OK;
}

inline DRESULT Write(BYTE, const BYTE*, DWORD, UINT) { return RES_WRPRT; }

inline DRESULT Ioctl(BYTE requested, BYTE command, void* buffer) {
    if (Status(requested) & STA_NOINIT)
        return RES_NOTRDY;
    if (command == CTRL_SYNC)
        return RES_OK;  // No write cache and no card command.
    if (!buffer)
        return RES_PARERR;
    switch (command) {
        case GET_SECTOR_COUNT:
            *static_cast<DWORD*>(buffer) = card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buffer) = card->csd.sector_size;
            return RES_OK;
        default:
            return RES_PARERR;  // Includes CTRL_TRIM: never erase/discard.
    }
}

inline const ff_diskio_impl_t kReadOnly = {Status, Status, Read, Write, Ioctl};
}  // namespace boilon::sd_probe
