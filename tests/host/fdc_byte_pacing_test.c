/* SPDX-License-Identifier: MIT
 * Synthetic controller regression: no game data and no guest-code patches. */
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <hostfile.h>
#include "../../components/pc98core/dev/fdc765.c"

static void tick(void) { const UINT32 before = g_cpu.clocks_done; do { g_cpu.slice_left = 0; ev_slice_done(); } while (g_cpu.clocks_done == before); }
static void setup(unsigned hz, unsigned duration, int nd)
{
    ev_reset_all();
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.slice_len = g_cpu.slice_left = EV_MAXCLOCK;
    g_mach.cpu_clock = hz; g_mach.clock_mul = 3;
    Pc98Settings cfg; memset(&cfg, 0, sizeof(cfg)); cfg.fdd_fitted = 3;
    fdc_reset(&cfg);
    dma_request(2, 0); dma_request(3, 0);
    fm_track_t *t = (fm_track_t *)fm_disk_track(fdd_media_disk(0), 0, 0);
    assert(t && t->nsect == 8);
    t->sec[0].data_ns = duration;
    F.kind = X_READ; F.nd = nd; F.mf = 1; F.us = 0; F.sec = 0;
    F.n = 3; F.r = 1; F.eot = 8;
    F.t = us_to_clk(30000);
    memset(F.buf, 0x5a, 1024);
    start_transfer(1024, 1);
}
static void check_read(unsigned hz, unsigned duration, int nd)
{
    setup(hz, duration, nd);
    const UINT64 origin = F.data_start, span = F.data_clocks;
    assert(!F.byte_ready && !(g_dma_active & 4));
    assert(to_host() == 0xff && F.pos == 0);
    UINT64 first = 0, last = 0;
    for (unsigned i = 0; i < 1024; i++) {
        tick();
        assert(F.byte_ready && F.pos == i);
        assert(now() == origin + span * (i + 1) / 1024);
        if (!i) first = now();
        last = now();
        if (nd) assert(F.irq_on && (status_r(0x90) & MSR_RQM));
        else assert(g_dma_active & 4);
        if (i == 1023) F.tc = 1;
        assert(to_host() == 0x5a);
        assert(!F.byte_ready && !(g_dma_active & 4));
        if (i < 1023 && nd) assert(!(status_r(0x90) & MSR_RQM));
    }
    assert(last - first == span - span / 1024);
    assert(!ev_armed(EV_FDCBYTE));
    while (F.phase != PH_RESULT) tick();
    assert(!(F.res[0] & ST0_ABN) && !F.res[1] && !F.res[2]);
}
int main(void)
{
    HFILE image = hf_create("/timing.hdm"); assert(image);
    assert(hf_seek(image, 77*2*8*1024-1, HF_SEEK_SET) >= 0);
    const unsigned char zero = 0; assert(hf_write(image, &zero, 1) == 1); hf_close(image);
    assert(drv_fd_mount(0, "/timing.hdm") == 0);
    for (unsigned mode = 0; mode < 2; mode++) {
        check_read(7372800, 0, mode);           /* ordinary 500 kbit/s media */
        check_read(7372800, 20480000, mode);    /* a 20 us/byte recording */
        check_read(18432000, 20480000, mode);   /* independent of guest CPU speed */
    }
    setup(7372800, 0, 0); tick(); tick();
    assert((F.st1 & ST1_OR) && !F.byte_ready && !(g_dma_active & 4));
    setup(7372800, 0, 0); tick(); F.tc = 1; (void)to_host();
    assert(!ev_armed(EV_FDCBYTE) && !(g_dma_active & 4));
    setup(7372800, 0, 0); reset_controller();
    assert(!ev_armed(EV_FDCBYTE) && !(g_dma_active & 4));
    setup(7372800, 20480000, 0); F.kind = X_WRITE;
    start_transfer(1024, 0);
    assert(F.data_clocks == (UINT64)16384000 * 7372800 / 1000000000u);
    for (unsigned i = 0; i < 1024; i++) {
        do { tick(); } while (!F.byte_ready);
        if (i == 1023) F.tc = 1;
        from_host((UINT8)(i * 13));
    }
    fm_disk_t *disk = fdd_media_disk(0);
    const fm_track_t *track = fm_disk_track(disk, 0, 0);
    UINT8 data[1024]; assert(fm_sector_read(disk, track, 0, data, sizeof(data)) == 1024);
    for (unsigned i = 0; i < 1024; i++) assert(data[i] == (UINT8)(i * 13));
    setup(7372800, 0, 0); F.kind = X_SCAN; F.cmd[0] = 0x11;
    for (unsigned i = 0; i < 1024; i++) { tick(); from_host(0x5a); }
    assert(F.st2 & ST2_SH);
    setup(7372800, 0, 0); start_transfer(0, 1);
    assert(F.st1 & ST1_ND);
    setup(7372800, 0, 0);
    drv_fd_eject(0);
    image = hf_create("/timing.hdm"); assert(image);
    assert(hf_seek(image, 1474560-1, HF_SEEK_SET) >= 0);
    assert(hf_write(image, &zero, 1) == 1); hf_close(image);
    assert(drv_fd_mount(0, "/timing.hdm") == 0);
    F.kind = X_FORMAT; F.sc = 8; F.n = 3; F.fill = 0xe5; F.t = now();
    start_transfer(32, 0);
    for (unsigned i = 0; i < 32; i++) {
        do { tick(); } while (!F.byte_ready);
        const UINT8 chrn[] = {0,0,(UINT8)(i/4+1),3}; from_host(chrn[i%4]);
    }
    assert(!(F.st0 & ST0_ABN));
    drv_fd_eject(0); assert(drv_fd_mount(0, "/timing.hdm") == 0);
    fm_geom_t geometry; fm_disk_geometry(fdd_media_disk(0), &geometry);
    assert(geometry.tracks == 154 && geometry.sectors == 8 && geometry.n == 3);
    disk = fdd_media_disk(0); track = fm_disk_track(disk, 0, 0);
    for (unsigned sector = 0; sector < 8; sector++) {
        assert(fm_sector_read(disk, track, sector, data, sizeof(data)) == 1024);
        for (unsigned i = 0; i < 1024; i++) assert(data[i] == 0xe5);
    }
    drv_fd_eject(0);
    puts("PASS: paced DMA/non-DMA, nominal/measured rates, CPU independence, overrun, TC, reset, write/scan, empty transfer, 1.44-to-1.25 MB format/reopen");
}
