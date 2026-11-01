// hostfile.h for ESP32 / SD - POSIX fd I/O version.
//
// IMPORTANT: the floppy and hard disk code opens/seeks/reads the disk image from inside
// machine_frame(). newlib *stdio* (fopen/fseek/fread on FILE*, which Arduino's
// fs::File uses) CRASHES when called from that context. POSIX open/read/lseek
// (raw fds, no FILE/reent) work, so this layer uses them directly against the
// SD VFS mountpoint ("/sd"). SD.begin() is still used (to mount) by main.

#include "util/pcstr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"

extern "C" int ets_printf(const char *fmt, ...);
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"   // esp_ptr_dma_capable()
// Pre-include the C++ standard headers compiler_base.h pulls in, so their include
// guards prevent re-inclusion (with C linkage) inside the extern "C" block below.
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <climits>
#include <csetjmp>
#include <cstdarg>
#include <cinttypes>
#include <string>
#include <memory>

extern "C" {
#include <compiler.h>
#include <hostfile.h>
}

extern "C" int ets_printf(const char *fmt, ...);

// Concrete handle: dosio.h declares `typedef struct RFILE *HFILE;`
// fd >= 0: a real file on the card. fd < 0: one of the built-in ROMs, served
// straight out of memory-mapped flash (see builtin_lookup below).
struct RFILE {
    int fd;
    const uint8_t *blob = nullptr;
    size_t blob_len = 0;
    size_t blob_pos = 0;
};

#define SD_MOUNT "/sd"     // Arduino SD default VFS mountpoint

// Map a core path ("/FD.NFD") to the SD VFS path ("/sd/FD.NFD").
static void map_path(char *out, size_t outsz, const char *path) {
    if (path && path[0] == '/')
        snprintf(out, outsz, "%s%s", SD_MOUNT, path);
    else
        snprintf(out, outsz, "%s/%s", SD_MOUNT, path ? path : "");
}

// ============================================================================
//  I/O worker task.
//
//  The disk code calls hf_open_rw/seek/read from *inside* machine_frame(), deep
//  in the emulated-CPU call stack. Calling the SD/FAT VFS syscalls from there
//  faults (LoadProhibited, NULL ctx in the FAT handler), even though the very
//  same calls work at setup time (shallow stack). To be safe regardless of the
//  exact cause, every real ::open/::lseek/::read/... runs on a dedicated worker
//  task with a shallow internal-RAM stack; the emulator hands off a request and
//  blocks until the worker finishes. One requester (the emulator) at a time.
// ============================================================================
enum {
    IOP_OPEN, IOP_SEEK, IOP_READ, IOP_WRITE, IOP_CLOSE,
    IOP_SYNC, IOP_FSIZE, IOP_ATTR, IOP_UNLINK, IOP_RENAME, IOP_MKDIR, IOP_RMDIR, IOP_TRUNC
};
struct IoReq {
    int op, fd, flags, whence;
    const char *path, *path2;
    void *buf; size_t len; off_t off;
    long result;
};
// 0 = run SD syscalls directly on the calling task (faster; safe now that the
// screen-draw overrun is fixed). Set to 1 to route them through a shallow worker
// task again (kept as a safety fallback).
#define USE_IO_WORKER 0

static IoReq            s_io;
static SemaphoreHandle_t s_io_req, s_io_done, s_io_mtx;

// ---- DMA bounce buffer for disk I/O ----------------------------------------
// Disk images are read straight into PSRAM (mem[], the FDD/HDD sector buffers).
// The SD driver cannot DMA out of PSRAM, so for every such transfer it tries to
// heap_caps_malloc() an internal DMA buffer the size of the transfer - and with
// the BLE controller holding most of the internal RAM that allocation fails:
//     E diskio_sdmmc: sdmmc_read_blocks failed (0x101 = ESP_ERR_NO_MEM)
// The BIOS then cannot read the IPL and the machine falls through to N88-BASIC,
// which looks exactly like "the SD card is not being read".
// One small permanent internal buffer, used in fixed-size chunks, removes every
// dynamic allocation from the disk path (and caps how much internal RAM disk I/O
// can ever need, whatever else is running).
// Allocated lazily, on the first PSRAM-destined transfer. That is deliberate:
// the I2S driver wants an 8KB DMA block of its own and installs itself AFTER
// hf_init() but BEFORE the first disk read, so claiming this buffer eagerly
// in hf_init() is what made i2s_driver_install() fail with "Error malloc dma
// buffer" (no audio). Internal DMA memory is scarce enough here that the order
// of these two allocations decides whether both fit.
#define SD_BOUNCE_SZ 2048
static uint8_t *s_bounce = nullptr;

static uint8_t *sd_bounce(void) {
    if (!s_bounce) {
        // 64-byte aligned: the SD driver wants cache-line alignment on a PSRAM
        // enabled S3 and allocates its own buffer when it does not get it.
        s_bounce = (uint8_t *)heap_caps_aligned_alloc(64, SD_BOUNCE_SZ,
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ets_printf("dosio: sd bounce buffer %s (%d bytes, %u internal DMA free)\n",
                   s_bounce ? "OK" : "FAILED", SD_BOUNCE_SZ,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    return s_bounce;
}

// The SD driver falls back to its own internal allocation for any buffer it
// cannot DMA from directly, and on a PSRAM-enabled S3 that means anything in
// PSRAM *or* anything not aligned to a 64-byte cache line. A 512-byte sector
// buffer sits in internal RAM but at a 16-byte-aligned address (0x3fcc6950), so
// it took that path too - and that allocation is exactly what runs out of memory
// once BLE and I2S have taken their share.
#define SD_DMA_ALIGN 64
static bool buf_needs_bounce(const void *p) {
    return p && (!esp_ptr_dma_capable(p) || ((uintptr_t)p & (SD_DMA_ALIGN - 1)));
}

static long read_bounced(int fd, void *dst, size_t len) {
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > SD_BOUNCE_SZ) chunk = SD_BOUNCE_SZ;
        long n = (long)::read(fd, s_bounce, chunk);
        if (n <= 0) return done ? (long)done : n;
        memcpy(out + done, s_bounce, (size_t)n);
        done += (size_t)n;
        if ((size_t)n < chunk) break;      // short read = EOF
    }
    return (long)done;
}

static long write_bounced(int fd, const void *src, size_t len) {
    const uint8_t *in = (const uint8_t *)src;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > SD_BOUNCE_SZ) chunk = SD_BOUNCE_SZ;
        memcpy(s_bounce, in + done, chunk);
        long n = (long)::write(fd, s_bounce, chunk);
        if (n <= 0) return done ? (long)done : n;
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    return (long)done;
}

// Temporary: log the first reads so a failing transfer can be matched to its
// buffer/length (set to 0 once the SD path is settled).
#define SD_IO_TRACE 0

// Read/write with the bounce buffer when the caller's buffer is in PSRAM.
static long io_read(int fd, void *buf, size_t len) {
    bool bounce = buf_needs_bounce(buf) && sd_bounce() != nullptr;
#if SD_IO_TRACE
    static int trace = 0;
    if (trace < 40) {
        trace++;
        ets_printf("sdio: read fd=%d buf=%p len=%u bounce=%d\n",
                   fd, buf, (unsigned)len, (int)bounce);
    }
#endif
    if (bounce) return read_bounced(fd, buf, len);
    return (long)::read(fd, buf, len);
}
static long io_write(int fd, const void *buf, size_t len) {
    if (buf_needs_bounce(buf) && sd_bounce()) return write_bounced(fd, buf, len);
    return (long)::write(fd, buf, len);
}

// Cut the file at the current position. The handle stays open in the cache,
// so the new length is synced now or FatFs never writes it to the directory.
static long io_trunc(int fd) {
    const off_t pos = ::lseek(fd, 0, SEEK_CUR);
    const long r = ::ftruncate(fd, pos);
    ::fsync(fd);
    ets_printf("dosio: truncate fd=%d at %ld -> %ld\n", fd, (long)pos, r);
    return r;
}

#if USE_IO_WORKER
static void io_task(void *) {
    for (;;) {
        xSemaphoreTake(s_io_req, portMAX_DELAY);
        IoReq &q = s_io;
        switch (q.op) {
        case IOP_OPEN:   q.result = ::open(q.path, q.flags, 0666); break;
        case IOP_SEEK:   q.result = (long)::lseek(q.fd, q.off, q.whence); break;
        case IOP_READ:   q.result = io_read(q.fd, q.buf, q.len); break;
        case IOP_WRITE:  q.result = io_write(q.fd, q.buf, q.len); break;
        case IOP_CLOSE:  q.result = ::close(q.fd); break;
        case IOP_SYNC:   q.result = ::fsync(q.fd); break;
        case IOP_FSIZE:  { struct stat st; q.result = (::fstat(q.fd, &st) == 0) ? (long)st.st_size : -1; } break;
        case IOP_ATTR:   { struct stat st; q.result = (::stat(q.path, &st) != 0) ? -1 : (S_ISDIR(st.st_mode) ? HF_ATTR_DIR : HF_ATTR_FILE); } break;
        case IOP_UNLINK: q.result = ::unlink(q.path); break;
        case IOP_RENAME: q.result = ::rename(q.path, q.path2); break;
        case IOP_MKDIR:  q.result = ::mkdir(q.path, 0777); break;
        case IOP_RMDIR:  q.result = ::rmdir(q.path); break;
        case IOP_TRUNC:  q.result = io_trunc(q.fd); break;
        default:         q.result = -1; break;
        }
        xSemaphoreGive(s_io_done);
    }
}
#endif // USE_IO_WORKER
// Perform one syscall. The io worker exists only as a fallback: the real root
// cause of the earlier exec-context crashes was a screen-draw buffer overrun
// (fixed by gating drawscreen), not the VFS itself, so direct calls from the
// exec stack are safe — and avoid a per-sector cross-core context switch, which
// noticeably speeds up disk-heavy phases (DOS loading COMMAND.COM etc.).
static long io_call(const IoReq &r) {
    if (!USE_IO_WORKER || !s_io_mtx) {   // direct (default)
        const IoReq &q = r;
        switch (q.op) {
        case IOP_OPEN:   return ::open(q.path, q.flags, 0666);
        case IOP_SEEK:   return (long)::lseek(q.fd, q.off, q.whence);
        case IOP_READ:   return io_read(q.fd, q.buf, q.len);
        case IOP_WRITE:  return io_write(q.fd, q.buf, q.len);
        case IOP_CLOSE:  return ::close(q.fd);
        case IOP_SYNC:   return ::fsync(q.fd);
        case IOP_FSIZE:  { struct stat st; return (::fstat(q.fd, &st) == 0) ? (long)st.st_size : -1; }
        case IOP_ATTR:   { struct stat st; return (::stat(q.path, &st) != 0) ? -1 : (S_ISDIR(st.st_mode) ? HF_ATTR_DIR : HF_ATTR_FILE); }
        case IOP_UNLINK: return ::unlink(q.path);
        case IOP_RENAME: return ::rename(q.path, q.path2);
        case IOP_MKDIR:  return ::mkdir(q.path, 0777);
        case IOP_RMDIR:  return ::rmdir(q.path);
        case IOP_TRUNC:  return io_trunc(q.fd);
        default:         return -1;
        }
    }
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    s_io = r;
    xSemaphoreGive(s_io_req);
    xSemaphoreTake(s_io_done, portMAX_DELAY);
    long res = s_io.result;
    xSemaphoreGive(s_io_mtx);
    return res;
}

// ============================================================================
//  Persistent handle cache.
//
//  Reads/writes reuse one fd per image (opened once), so ::open never runs from
//  the exec context and per-sector open/close overhead disappears.
// ============================================================================
#define OPEN_CACHE_SLOTS 4
struct CacheSlot { char path[MAX_PATH + 8]; int fd; };
static CacheSlot s_cache[OPEN_CACHE_SLOTS];
static bool s_cache_initialized;

static void cache_init(void) {
    if (s_cache_initialized) return;
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++) s_cache[i].fd = -1;
    s_cache_initialized = true;
}

// Return an fd for `full`, opening (once) O_RDWR with O_RDONLY fallback so the
// same fd serves both reads and writes. Cached across calls; -1 on failure.
static int cache_open(const char *full) {
    cache_init();
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++)
        if (s_cache[i].fd >= 0 && strcmp(s_cache[i].path, full) == 0)
            return s_cache[i].fd;
    IoReq r{}; r.op = IOP_OPEN; r.path = full; r.flags = O_RDWR;
    int fd = (int)io_call(r);
    if (fd < 0) { r.flags = O_RDONLY; fd = (int)io_call(r); }
    if (fd < 0) return -1;
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++) {
        if (s_cache[i].fd < 0) {
            snprintf(s_cache[i].path, sizeof(s_cache[i].path), "%s", full);
            s_cache[i].fd = fd;
            return fd;
        }
    }
    // Cache full: usable but won't be reused (shouldn't happen for <=4 images).
    return fd;
}
// True if `fd` belongs to the cache (→ hf_close must not really close it).
static bool cache_owns(int fd) {
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++)
        if (s_cache[i].fd == fd) return true;
    return false;
}

extern "C" {

void hf_init(void) {
    cache_init();
#if USE_IO_WORKER
    // Optional fallback worker (shallow internal-RAM stack, other core).
    s_io_req  = xSemaphoreCreateBinary();
    s_io_done = xSemaphoreCreateBinary();
    s_io_mtx  = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(io_task, "sdio", 16 * 1024, nullptr, 10, nullptr, 0);
#endif
}
void hf_term(void) {
    if (!s_cache_initialized) return;
    // hf_close intentionally retains cached handles. They must be synced and
    // closed before unmount/reboot, including the final dirty data/FAT sector.
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++) {
        if (s_cache[i].fd < 0) continue;
        IoReq r{}; r.fd = s_cache[i].fd; r.op = IOP_SYNC;
        if (io_call(r) != 0) ets_printf("dosio: sync failed for %s\n", s_cache[i].path);
        r.op = IOP_CLOSE; io_call(r);
        s_cache[i].fd = -1; s_cache[i].path[0] = 0;
    }
}

// ---- built-in ROMs --------------------------------------------------------
//
// The compatible BIOS and font are linked into the firmware and handed out
// here as read-only files. Doing it at this layer means nothing above has to
// know: rom.c and cgrom.c open a path and read it, and
// whether that path is on the SD card or in flash is not their business. The
// menu's ROM switching keeps working unchanged - it just names a real file
// instead, and a real file wins.
//
// The names cannot collide with anything on the card: no SD path starts with
// a colon.
#define BUILTIN_BIOS ":builtin/BIOS.ROM"
#define BUILTIN_FONT ":builtin/FONT.ROM"
#define BUILTIN_HDDROM ":builtin/HDDROM.BIN"   // rom_src/hddrom: IDE option ROM for dumped BIOSes

// The built-in BIOS (rom_src/pc98_bios_native). If no device boots it halts
// where you can see it. Nothing in it derives from NEC's ROM, and it
// carries no NEC copyright notice - so NEC's own MS-DOS, which checks for one,
// needs a BIOS.ROM dumped from an NEC machine instead.
extern const uint8_t builtin_bios_start[] asm("_binary_PC98N_ROM_start");
extern const uint8_t builtin_bios_end[]   asm("_binary_PC98N_ROM_end");
extern const uint8_t builtin_font_start[] asm("_binary_FONT_ESP_ROM_start");
extern const uint8_t builtin_font_end[]   asm("_binary_FONT_ESP_ROM_end");
extern const uint8_t builtin_hddrom_start[] asm("_binary_HDDROM_BIN_start");
extern const uint8_t builtin_hddrom_end[]   asm("_binary_HDDROM_BIN_end");

static const uint8_t *builtin_lookup(const char *path, size_t *len) {
    if (!path) {
        return nullptr;
    }
    if (!strcmp(path, BUILTIN_BIOS)) {
        *len = (size_t)(builtin_bios_end - builtin_bios_start);
        return builtin_bios_start;
    }
    if (!strcmp(path, BUILTIN_FONT)) {
        *len = (size_t)(builtin_font_end - builtin_font_start);
        return builtin_font_start;
    }
    if (!strcmp(path, BUILTIN_HDDROM)) {
        *len = (size_t)(builtin_hddrom_end - builtin_hddrom_start);
        return builtin_hddrom_start;
    }
    return nullptr;
}

// ---- open / create ----
// Reads/writes of existing files go through the persistent cache (see above).
static HFILE open_cached(const char *path) {
    {   // a built-in ROM? Then there is no file and no SD access at all.
        size_t blen = 0;
        const uint8_t *blob = builtin_lookup((const char *)path, &blen);
        if (blob) {
            RFILE *h = new RFILE();
            h->fd = -1;
            h->blob = blob;
            h->blob_len = blen;
            h->blob_pos = 0;
            return (HFILE)h;
        }
    }
    char full[MAX_PATH + 8];
    map_path(full, sizeof(full), path);
    int fd = cache_open(full);
    if (fd < 0) return NULL;
    RFILE *h = new RFILE();
    h->fd = fd;
    return (HFILE)h;
}
HFILE hf_open_rw(const char *path)    { return open_cached(path); }
HFILE hf_open(const char *path) { return open_cached(path); }
// Create bypasses the cache (fresh O_CREAT|O_TRUNC fd, closed normally).
HFILE hf_create(const char *path)  {
    char full[MAX_PATH + 8];
    map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_OPEN; r.path = full; r.flags = O_RDWR | O_CREAT | O_TRUNC;
    int fd = (int)io_call(r);
    if (fd < 0) return NULL;
    RFILE *h = new RFILE();
    h->fd = fd;
    return (HFILE)h;
}

// ---- seek / io / close (all real syscalls go through the io worker) ----
long hf_seek(HFILE handle, long pointer, int method) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    if (h->fd < 0) {
        long p = (method == HF_SEEK_CUR) ? (long)h->blob_pos + pointer
               : (method == HF_SEEK_END) ? (long)h->blob_len + pointer
                                       : (long)pointer;
        if (p < 0) p = 0;
        if (p > (long)h->blob_len) p = (long)h->blob_len;
        h->blob_pos = (size_t)p;
        return (long)p;
    }
    int w = (method == HF_SEEK_CUR) ? SEEK_CUR : (method == HF_SEEK_END) ? SEEK_END : SEEK_SET;
    IoReq r{}; r.op = IOP_SEEK; r.fd = h->fd; r.off = pointer; r.whence = w;
    long res = io_call(r);
    return (long)(res < 0 ? 0 : res);
}
UINT hf_read(HFILE handle, void *data, UINT length) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    if (h->fd < 0) {
        size_t n = h->blob_len - h->blob_pos;
        if (n > length) n = length;
        memcpy(data, h->blob + h->blob_pos, n);   // flash is memory-mapped
        h->blob_pos += n;
        return (UINT)n;
    }
    UINT done = 0;
    while (done < length) {
        IoReq r{}; r.op = IOP_READ; r.fd = h->fd;
        r.buf = (uint8_t *)data + done; r.len = length - done;
        long n = io_call(r);
        if (n <= 0) break;
        done += (UINT)n;
    }
    return done;
}
UINT hf_write(HFILE handle, const void *data, UINT length) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    if (h->fd < 0) return 0;                      // built-in ROMs are read-only
    if (length == 0) {                  // length 0: the file ends here
        IoReq r{}; r.op = IOP_TRUNC; r.fd = h->fd;
        return io_call(r) == 0 ? 0 : (UINT)-1;
    }
    UINT done = 0;                      // loop: a short write is not an error
    while (done < length) {
        IoReq r{}; r.op = IOP_WRITE; r.fd = h->fd;
        r.buf = (void *)((const uint8_t *)data + done); r.len = length - done;
        long n = io_call(r);
        if (n <= 0) break;
        done += (UINT)n;
    }
    return done;
}
short hf_close(HFILE handle) {
    RFILE *h = (RFILE *)handle;
    if (!h) return -1;
    if (h->fd < 0) { delete h; return 0; }
    if (!cache_owns(h->fd)) {           // keep cached fds open
        IoReq r{}; r.op = IOP_CLOSE; r.fd = h->fd; io_call(r);
    }
    delete h;
    return 0;
}
long hf_size(HFILE handle) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    if (h->fd < 0) return (long)h->blob_len;
    IoReq r{}; r.op = IOP_FSIZE; r.fd = h->fd;
    long sz = io_call(r);
    return (long)(sz < 0 ? 0 : sz);
}

// ---- path-based ops (via io worker) ----
short hf_delete(const char *path) {
    char full[MAX_PATH + 8]; map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_UNLINK; r.path = full;
    return (io_call(r) == 0) ? 0 : -1;
}
short hf_attr(const char *path) {
    char full[MAX_PATH + 8]; map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_ATTR; r.path = full;
    return (short)io_call(r);
}

} // extern "C"
