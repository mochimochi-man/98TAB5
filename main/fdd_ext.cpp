// Debug routine.
#include "util/pcstr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include <compiler.h>
#include <pc98.h>
#include <cpulink.h>
#include <devices.h>

#include <media/fdmedia.h>
#include <media/drives.h>

extern "C" int ets_printf(const char *fmt, ...);

#define XD_DRIVES     2
#define XD_TRACKS     168
#define XD_MAX_SEC    32
#define XD_NDIAG      8
#define XD_TRK_MAGIC  0x4b525446u
#define XD_DOOR_FRAMES 30

static const char s_magic[8] = { 0x50, 0x43, 0x39, 0x38, 0x46, 0x54, 0x4b, 0x31 };
static const char s_ext[] = { 0x2e, 0x66, 0x74, 0x6b, 0 };

#pragma pack(push, 1)
struct xd_head {
    char     magic[8];
    uint32_t ntrk;
    uint32_t reserved;
    uint32_t toff[XD_TRACKS];
};

struct xd_diag {
    uint32_t off;
    uint32_t len;
    int32_t  crc_bad;
};

struct xd_trkh {
    uint32_t magic;
    uint16_t nsec;
    uint16_t flags;
    xd_diag  diag[XD_NDIAG];
};

struct xd_sec {
    uint8_t  c, h, r, n;
    uint8_t  id_ok, data_ok, deleted, fm;
    uint8_t  seen, no_data, dam_gap, pad;
    uint16_t len;
    uint16_t pad2;
    uint32_t data_off;
    uint32_t pos_us;
    uint32_t rev_us;
    uint32_t byte_ns;
    uint32_t data_ns;
    uint32_t data_offset_ns;
};
#pragma pack(pop)

struct xd_trk {
    xd_sec   *sec;
    int       nsec;
    bool      loaded;
    uint8_t **ov;
};

struct xd_drv {
    xd_trk     trk[XD_TRACKS];
    uint32_t   toff[XD_TRACKS];
    FILE      *fp;
    bool       mounted;
    int        cyls, spt, secsize;
    fm_disk_t *disk;
};

EXT_RAM_BSS_ATTR static xd_drv s_dr[XD_DRIVES];

static bool s_machine_running;
static int  s_open_frames[XD_DRIVES];

static inline xd_drv *drv_of(int d) {
    return (d >= 0 && d < XD_DRIVES) ? &s_dr[d] : nullptr;
}

static bool at(xd_drv *d, uint32_t off, void *dst, size_t n) {
    return d->fp && fseek(d->fp, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, d->fp) == n;
}

static void free_track(xd_trk &t) {
    if (t.ov) {
        for (int k = 0; k < t.nsec; k++) {
            free(t.ov[k]);
        }
        free(t.ov);
        t.ov = nullptr;
    }
    free(t.sec);
    t.sec = nullptr;
    t.nsec = 0;
    t.loaded = false;
}

static bool open_file(xd_drv *d, int drive, const char *path) {
    EXT_RAM_BSS_ATTR static xd_head h;
    d->fp = fopen(path, "rb");
    if (!d->fp) {
        ets_printf("fdd%d: cannot open %s\n", drive + 1, path);
        return false;
    }
    if (!at(d, 0, &h, sizeof h) || memcmp(h.magic, s_magic, 8) != 0 || h.ntrk != XD_TRACKS) {
        ets_printf("fdd%d: %s is not a disk image\n", drive + 1, path);
        fclose(d->fp);
        d->fp = nullptr;
        return false;
    }
    memcpy(d->toff, h.toff, sizeof d->toff);
    return true;
}

static int track_load(xd_drv *d, int drive, int trk) {
    if (trk < 0 || trk >= XD_TRACKS) {
        return -1;
    }
    xd_trk &t = d->trk[trk];
    if (t.loaded) {
        return t.nsec > 0;
    }
    const uint32_t base = d->toff[trk];
    xd_sec *sec = nullptr;
    int n = 0;
    if (base) {
        xd_trkh h;
        if (!at(d, base, &h, sizeof h) || h.magic != XD_TRK_MAGIC) {
            ets_printf("fdd%d: track %d unreadable\n", drive + 1, trk);
            return -1;
        }
        n = h.nsec < XD_MAX_SEC ? h.nsec : XD_MAX_SEC;
        if (n > 0) {
            sec = (xd_sec *)heap_caps_malloc(sizeof(xd_sec) * n, MALLOC_CAP_SPIRAM);
            if (!sec || !at(d, base + sizeof h, sec, sizeof(xd_sec) * n)) {
                free(sec);
                ets_printf("fdd%d: track %d unreadable\n", drive + 1, trk);
                return -1;
            }
            for (int k = 0; k < n; k++) {
                sec[k].data_off += base;
            }
        }
    }
    t.sec = sec;
    t.nsec = n;
    t.ov = nullptr;
    t.loaded = true;
    return n > 0;
}

static int sector_bytes(xd_drv *d, const xd_trk &t, int idx, uint8_t *dst, size_t n) {
    const xd_sec &p = t.sec[idx];
    if (n > p.len) {
        n = p.len;
    }
    if (t.ov && t.ov[idx]) {
        memcpy(dst, t.ov[idx], n);
    } else if (!at(d, p.data_off, dst, n)) {
        return -1;
    }
    return (int)n;
}

static const fm_io_t s_io = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

static inline int disk_drive(const fm_disk_t *fd) {
    return (int)(intptr_t)fd->priv;
}

static void xd_close(fm_disk_t *fd) {
    (void)fd;
}

static int xd_flush(fm_disk_t *fd) {
    (void)fd;
    return FM_OK;
}

static int xd_compat(const fm_disk_t *fd, const fm_cfg_t *cfg) {
    (void)fd;
    return cfg->media == FM_MEDIA_2HD && cfg->hd_rot == FM_ROT_360;
}

static void xd_geometry(const fm_disk_t *fd, fm_geom_t *g) {
    (void)fd;
    g->media = FM_MEDIA_2HD;
    g->hd_rot = FM_ROT_360;
    g->uniform = 0;
    g->tracks = 160;
}

static int xd_load_track(fm_disk_t *fd, int cyl, int head, fm_track_t *out) {
    const int drive = disk_drive(fd);
    xd_drv *d = drv_of(drive);
    const int trk = cyl * 2 + head;

    if (!d || !d->mounted || cyl < 0 || head < 0 || head > 1 || trk >= XD_TRACKS) {
        return -1;
    }
    track_load(d, drive, trk);
    const xd_trk &t = d->trk[trk];
    int n = t.loaded ? t.nsec : 0;
    if (n > FM_MAX_SECTORS) {
        n = FM_MAX_SECTORS;
    }
    out->nsect = n;
    out->media = FM_MEDIA_2HD;
    out->hd_rot = FM_ROT_360;
    out->rev_us = 0;
    out->byte_ns = 0;
    for (int i = 0; i < n; i++) {
        const xd_sec &p = t.sec[i];
        fm_sector_t &s = out->sec[i];

        memset(&s, 0, sizeof(s));
        s.c = p.c;
        s.h = p.h;
        s.r = p.r;
        s.n = p.n;
        s.fm = p.fm ? 1 : 0;
        s.id_crc_ok = p.id_ok ? 1 : 0;
        s.data_crc_ok = p.data_ok ? 1 : 0;
        s.deleted = p.deleted ? 1 : 0;
        s.no_data = p.no_data ? 1 : 0;
        s.rot = FM_ROT_ANY;
        s.data_len = p.len;
        s.data_ns = p.data_ns;
        s.data_offset_ns = p.data_offset_ns;
        if (!s.data_offset_ns && !p.fm && !p.no_data && p.dam_gap != 255)
            s.data_offset_ns = (22u + p.dam_gap) * p.byte_ns;
        s.pos_us = p.pos_us;
        s.tag = (uint32_t)i;
        if (!out->rev_us && p.rev_us) {
            out->rev_us = p.rev_us;
        }
        if (p.byte_ns && (!out->byte_ns || !p.fm)) {
            out->byte_ns = p.fm ? p.byte_ns / 2 : p.byte_ns;
        }
    }
    return 0;
}

static int xd_read_data(fm_disk_t *fd, const fm_track_t *t, int idx, uint8_t *dst, size_t cap) {
    xd_drv *d = drv_of(disk_drive(fd));
    const int trk = t->cyl * 2 + t->head;

    if (!d || trk < 0 || trk >= XD_TRACKS || idx < 0 || idx >= d->trk[trk].nsec) {
        return -1;
    }
    return sector_bytes(d, d->trk[trk], idx, dst, cap);
}

static int xd_write_data(fm_disk_t *fd, const fm_track_t *t, int idx, const uint8_t *src, size_t len) {
    xd_drv *d = drv_of(disk_drive(fd));
    const int trk = t->cyl * 2 + t->head;

    if (!d || trk < 0 || trk >= XD_TRACKS || idx < 0 || idx >= d->trk[trk].nsec) {
        return FM_E_NOTRACK;
    }
    xd_trk &tr = d->trk[trk];
    const xd_sec &p = tr.sec[idx];
    if (len > p.len) {
        len = p.len;
    }
    if (!tr.ov) {
        tr.ov = (uint8_t **)calloc(tr.nsec, sizeof(uint8_t *));
    }
    if (tr.ov && !tr.ov[idx]) {
        uint8_t *b = (uint8_t *)heap_caps_malloc(p.len ? p.len : 1, MALLOC_CAP_SPIRAM);
        if (b && sector_bytes(d, tr, idx, b, p.len) < 0) {
            free(b);
            b = nullptr;
        }
        tr.ov[idx] = b;
    }
    if (!tr.ov || !tr.ov[idx]) {
        return FM_E_NOTRACK;
    }
    memcpy(tr.ov[idx], src, len);
    return FM_OK;
}

static int xd_set_marks(fm_disk_t *fd, const fm_track_t *t, int idx, int deleted) {
    xd_drv *d = drv_of(disk_drive(fd));
    const int trk = t->cyl * 2 + t->head;

    if (!d || trk < 0 || trk >= XD_TRACKS || idx < 0 || idx >= d->trk[trk].nsec) {
        return FM_E_NOTRACK;
    }
    xd_sec &p = d->trk[trk].sec[idx];
    p.data_ok = 1;
    p.deleted = deleted ? 1 : 0;
    p.no_data = 0;
    if (t == &fd->cur) {
        fd->cur.sec[idx].data_crc_ok = 1;
        fd->cur.sec[idx].deleted = p.deleted;
        fd->cur.sec[idx].no_data = 0;
    }
    return FM_OK;
}

static int xd_format(fm_disk_t *fd, int cyl, int head, const fm_cfg_t *cfg, const fm_format_t *f) {
    (void)fd; (void)cyl; (void)head; (void)cfg; (void)f;
    return FM_E_PROTECT;
}

static int xd_read_diag(fm_disk_t *fd, int cyl, int head, int ncode, uint8_t *out, size_t cap, int *crc_bad) {
    const int drive = disk_drive(fd);
    xd_drv *d = drv_of(drive);
    const int trk = cyl * 2 + head;

    if (!d || !d->mounted || trk < 0 || trk >= XD_TRACKS) {
        return -1;
    }
    if (track_load(d, drive, trk) <= 0) {
        return 0;
    }
    xd_trkh h;
    if (!at(d, d->toff[trk], &h, sizeof h) || h.magic != XD_TRK_MAGIC) {
        return 0;
    }
    const xd_diag &g = h.diag[ncode & (XD_NDIAG - 1)];
    const size_t n = g.len < cap ? g.len : cap;
    if (!n || !at(d, d->toff[trk] + g.off, out, n)) {
        return 0;
    }
    *crc_bad = g.crc_bad;
    return (int)n;
}

static const fm_backend_t s_ops = {
    "EXT", xd_close, xd_flush, xd_compat, nullptr,
    xd_load_track, nullptr, xd_read_data, xd_write_data, xd_format, xd_geometry,
    xd_set_marks, xd_read_diag
};

static void forget(int drive) {
    xd_drv *d = drv_of(drive);
    if (!d) {
        return;
    }
    for (int i = 0; i < XD_TRACKS; i++) {
        free_track(d->trk[i]);
    }
    if (d->fp) {
        fclose(d->fp);
        d->fp = nullptr;
    }
    drv_fd_set_external(drive, nullptr, nullptr);
    if (d->disk) {
        fm_disk_free(d->disk);
        d->disk = nullptr;
    }
    d->cyls = d->spt = d->secsize = 0;
    d->mounted = false;
}

static void on_eject(int drv) {
    forget(drv);
}

static void door_opened(int drive) {
    if (drive < 0 || drive >= XD_DRIVES || s_open_frames[drive] > 0) {
        return;
    }
    drv_fd_door(drive, 1);
    s_open_frames[drive] = XD_DOOR_FRAMES;
    fdc_drive_changed(drive, 0);
}

extern "C" bool fdd_ext_is(const char *name) {
    const char *dot = name ? strrchr(name, '.') : nullptr;
    return dot && strcasecmp(dot, s_ext) == 0;
}

extern "C" bool fdd_ext_mount(int drv, const char *path) {
    xd_drv *d = drv_of(drv);
    if (!d || drv >= DRV_FDD_MAX || !path || !path[0]) {
        return false;
    }
    drv_fd_eject(drv);
    forget(drv);

    char full[MAX_PATH + 8];
    snprintf(full, sizeof(full), "/sd%s", path);
    if (!open_file(d, drv, full)) {
        return false;
    }
    d->mounted = true;
    if (track_load(d, drv, 0) <= 0) {
        ets_printf("fdd%d: %s: cylinder 0 unreadable\n", drv + 1, path);
        forget(drv);
        return false;
    }

    int spt = 0;
    const int ncode = d->trk[0].sec[0].n;
    for (int i = 0; i < d->trk[0].nsec; i++) {
        const xd_sec *p = &d->trk[0].sec[i];
        if (p->n == ncode && p->r > spt && p->r <= 32) {
            spt = p->r;
        }
    }
    d->cyls = (ncode == 3 && spt == 8) ? 77 : 80;
    d->spt = spt;
    d->secsize = 128 << ncode;

    pcs_copy((char *)g_settings.fdd_image[drv], path, NELEMENTS(g_settings.fdd_image[drv]));

    d->disk = fm_disk_alloc(&s_ops, &s_io, "ext");
    if (!d->disk) {
        forget(drv);
        return false;
    }
    d->disk->priv = (void *)(intptr_t)drv;
    d->disk->protect = 0;
    drv_fd_set_external(drv, d->disk, on_eject);
    s_open_frames[drv] = 0;
    if (s_machine_running) {
        door_opened(drv);
    }
    return true;
}

extern "C" bool fdd_ext_mounted(int drv) {
    const xd_drv *d = drv_of(drv);
    return d && d->mounted;
}

extern "C" void fdd_ext_started(void) {
    s_machine_running = true;
}

extern "C" void fdd_ext_tick(void) {
    for (int d = 0; d < XD_DRIVES; d++) {
        xd_drv *p = drv_of(d);

        if (!p || !p->mounted || s_open_frames[d] <= 0) {
            continue;
        }
        if (--s_open_frames[d] == 0) {
            drv_fd_door(d, 0);
            fdc_drive_changed(d, 1);
        }
    }
}

extern "C" void fdd_ext_shutdown(void) {
    for (int d = 0; d < XD_DRIVES; d++) {
        if (s_dr[d].fp) {
            fclose(s_dr[d].fp);
            s_dr[d].fp = nullptr;
        }
    }
}
