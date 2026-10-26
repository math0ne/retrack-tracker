// OpenTracker -- FatFs disk glue + IStorage (spec 01 §6). FatFs R0.15a is vendored
// in third_party/fatfs (1-clause BSD-style licence).
#include "sd_storage.h"
#include "diag.h"

#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "sdhc.h"

extern "C" {
#include "ff.h"
#include "diskio.h"
}

// ---- FatFs disk I/O layer (FatFs "diskio.h" contract) ------------------------
extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!ot::hal::sdhc::cardPresent()) return STA_NOINIT | STA_NODISK;
    return ot::hal::sdhc::ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!ot::hal::sdhc::ready()) ot::hal::sdhc::init();
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!ot::hal::sdhc::ready()) return RES_NOTRDY;
    return ot::hal::sdhc::readBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!ot::hal::sdhc::ready()) return RES_NOTRDY;
    return ot::hal::sdhc::writeBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    if (!ot::hal::sdhc::ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;   // writes complete synchronously
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = ot::hal::sdhc::sectorCount(); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

}  // extern "C"

namespace ot {
namespace hal {

namespace {
FATFS gFs;
constexpr int kMaxFiles = 14;   // stem export keeps 11 WAVs open (spec 04 §6.1)
FIL gFiles[kMaxFiles];
bool gUsed[kMaxFiles];
char gPath[FF_MAX_LFN + 4];

// IStorage paths are relative to the card root; FatFs wants "/..." (no drive prefix needed).
const char* fix(const char* p) {
    const char* s = (p[0] == '/') ? p + 1 : p;
    size_t n = strlen(s);
    if (n + 2 > sizeof(gPath)) n = sizeof(gPath) - 2;
    gPath[0] = '/';
    memcpy(gPath + 1, s, n);
    gPath[n + 1] = 0;
    return gPath;
}
FIL* fileFor(IStorage::Handle h) {
    if (h < 0 || h >= kMaxFiles || !gUsed[h]) return nullptr;
    return &gFiles[h];
}
}  // namespace

bool SdStorage::mount() {
    if (storageMode_) return false;
    if (mounted_) return true;
    if (!sdhc::cardPresent()) return false;
    diagLog("sd: SDHC init (CMD0/8/ACMD41/2/3/9/7/ACMD6)");
    if (!sdhc::init()) {
        diagLogf("sd: SDHC init failed, last err 0x%08lX", static_cast<unsigned long>(sdhc::lastError()));
        return false;
    }
    const sdhc::Info& in = sdhc::info();
    diagLogf("sd: SDHC ready, %lu sectors, %s, %s timing, SDCLK %lu Hz%s%s; f_mount",
             static_cast<unsigned long>(sdhc::sectorCount()), in.adma ? "ADMA2" : "PIO",
             in.highSpeed ? "high-speed" : "default-speed", static_cast<unsigned long>(in.clockHz),
             sdhc::initNote()[0] ? " -- " : "", sdhc::initNote());
    FRESULT fr = f_mount(&gFs, "", 1);
    diagLogf("sd: f_mount result %d", int(fr));
    mounted_ = fr == FR_OK;
    return mounted_;
}

void SdStorage::unmount() {
    for (int i = 0; i < kMaxFiles; i++) {
        if (gUsed[i] && mounted_) f_close(&gFiles[i]);
        gUsed[i] = false;
    }
    if (mounted_) f_mount(nullptr, "", 0);
    mounted_ = false;
    sdhc::release();
}

void SdStorage::setStorageMode(bool on) {
    // spec 01 §6.1: storage mode = release the card (physical hand-over unknown).
    storageMode_ = on;
    if (on) unmount();
}

void SdStorage::service() {
    // spec 01 §6 card detect, 500 ms debounce.
    bool present = sdhc::cardPresent();
    uint32_t now = millis();
    if (present != pendingPresent_) { pendingPresent_ = present; changeMs_ = now; }
    if (present != lastPresent_ && now - changeMs_ >= board::kCardDebounceMs) {
        lastPresent_ = present;
        if (present) mount(); else unmount();
    }
}

IStorage::Handle SdStorage::open(const char* path, OpenMode mode) {
    if (!mounted_) return -1;
    int slot = -1;
    for (int i = 0; i < kMaxFiles; i++) if (!gUsed[i]) { slot = i; break; }
    if (slot < 0) return -1;
    BYTE m = 0;
    switch (mode) {
    case kRead: m = FA_READ | FA_OPEN_EXISTING; break;
    case kWrite: m = FA_WRITE | FA_CREATE_ALWAYS; break;
    case kReadWrite: m = FA_READ | FA_WRITE | FA_OPEN_ALWAYS; break;
    default: return -1;
    }
    if (f_open(&gFiles[slot], fix(path), m) != FR_OK) return -1;
    gUsed[slot] = true;
    return slot;
}

int32_t SdStorage::read(Handle h, void* dst, uint32_t len) {
    FIL* f = fileFor(h);
    if (!f) return -1;
    UINT br = 0;
    return f_read(f, dst, len, &br) == FR_OK ? int32_t(br) : -1;
}

int32_t SdStorage::write(Handle h, const void* src, uint32_t len) {
    FIL* f = fileFor(h);
    if (!f) return -1;
    UINT bw = 0;
    return f_write(f, src, len, &bw) == FR_OK ? int32_t(bw) : -1;
}

bool SdStorage::seek(Handle h, uint32_t pos) {
    FIL* f = fileFor(h);
    return f && f_lseek(f, pos) == FR_OK;
}

int32_t SdStorage::size(Handle h) {
    FIL* f = fileFor(h);
    return f ? int32_t(f_size(f)) : -1;
}

void SdStorage::close(Handle h) {
    FIL* f = fileFor(h);
    if (!f) return;
    f_close(f);
    gUsed[h] = false;
}

bool SdStorage::exists(const char* path) {
    if (!mounted_) return false;
    const char* p = fix(path);
    if (p[1] == 0) return true;   // root
    FILINFO fi;
    return f_stat(p, &fi) == FR_OK;
}

bool SdStorage::mkdir(const char* path) {
    if (!mounted_) return false;
    FRESULT r = f_mkdir(fix(path));
    return r == FR_OK || r == FR_EXIST;
}

bool SdStorage::remove(const char* path) {
    return mounted_ && f_unlink(fix(path)) == FR_OK;
}

bool SdStorage::rename(const char* from, const char* to) {
    if (!mounted_) return false;
    static char fromBuf[FF_MAX_LFN + 4];
    strncpy(fromBuf, fix(from), sizeof(fromBuf) - 1);
    fromBuf[sizeof(fromBuf) - 1] = 0;
    return f_rename(fromBuf, fix(to)) == FR_OK;
}

bool SdStorage::benchmark(uint8_t* buf, uint32_t bufLen, uint32_t totalBytes, Bench& r) {
    r = Bench();
    if (!mounted_ || !buf || bufLen < 512 || totalBytes < bufLen) return false;
    static const char kPath[] = "/OT_BENCH.TMP";
    FIL f;
    if (f_open(&f, kPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    const uint32_t chunks = totalBytes / bufLen;
    bool ok = true;
    memset(buf, 0x5A, bufLen);
    uint32_t t0 = micros();
    for (uint32_t i = 0; i < chunks && ok; i++) {
        memcpy(buf, &i, sizeof i);   // chunk tag, checked on read-back
        UINT bw = 0;
        ok = f_write(&f, buf, bufLen, &bw) == FR_OK && bw == bufLen;
    }
    ok = (f_sync(&f) == FR_OK) && ok;
    r.writeUs = micros() - t0;
    f_close(&f);
    if (ok && f_open(&f, kPath, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
        t0 = micros();
        for (uint32_t i = 0; i < chunks && ok; i++) {
            UINT br = 0;
            ok = f_read(&f, buf, bufLen, &br) == FR_OK && br == bufLen;
            uint32_t tag;
            memcpy(&tag, buf, sizeof tag);
            if (ok && (tag != i || buf[bufLen - 1] != 0x5A)) { r.verifyErrors++; }
        }
        r.readUs = micros() - t0;
        f_close(&f);
    } else {
        ok = false;
    }
    f_unlink(kPath);
    r.bytes = chunks * bufLen;
    r.ok = ok && r.verifyErrors == 0;
    return r.ok;
}

int32_t SdStorage::listDir(const char* path, DirFn fn, void* ctx) {
    if (!mounted_) return -1;
    DIR d;
    if (f_opendir(&d, fix(path)) != FR_OK) return -1;
    int32_t n = 0;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        n++;
        DirEntry e{fi.fname, (fi.fattrib & AM_DIR) != 0, uint32_t(fi.fsize)};
        if (fn && !fn(e, ctx)) break;
    }
    f_closedir(&d);
    return n;
}

}  // namespace hal
}  // namespace ot
