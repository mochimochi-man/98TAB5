/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Stage 13-0 measurement (PC98_PROF=1 only): opcode / prefix / ModR/M / path / mode / exception / port counters.
 * Counting runs in the adapter loop (one instruction per fast16 call while profiling); output is one batch
 * on request (PROF lines), never from the render loop. */
#ifdef PC98_PROF
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <string.h>
#include "cpuadapt.h"
#include "cpuprof.h"
#include "esp_attr.h"

extern int ets_printf(const char *fmt, ...);

unsigned long long g_prof_evt[PEV_MAX];
static unsigned long long p_main[3][3][256];     /* [menu][path 0 fast16, 1 ref (fast16 refused), 2 ref (ineligible)][opcode] */
static unsigned long long p_0f[3][3][256];
static unsigned long long p_mod[3][256][4];      /* ModR/M mod of the byte after the opcode (every opcode; has-modrm filtered offline) */
static unsigned long long p_reg[3][256][8];      /* reg field, likewise (groups: 80 81 83 C0 C1 D0-D3 F6 F7 FE FF) */
static unsigned long long p_0freg[3][256][8];
static unsigned long long p_pfx[3][256];
static unsigned long long p_rep[3][256];         /* REP/REPNE-prefixed string op iterations, by opcode */
static unsigned long long p_intn[3][256];        /* INT n (CD) vector */
static unsigned long long p_fault[3][32];        /* fault vector delivered through the reference */
static unsigned long long p_trap[3];
static unsigned long long p_irq[3][256];
static unsigned long long p_stop[3];
static unsigned long long p_pe_steps[3], p_pe_cpl[3][4], p_total[3];
static unsigned long long p_cpl_tr[4][4], p_o32[3], p_a32[3], p_hlt[3], p_codeunread[3];
EXT_RAM_BSS_ATTR static uint32_t p_port[2][65536];   /* [0 in, 1 out] 8/16-bit alike */

static struct { unsigned menu, op, op2, modrm, has2, rep, pe, cpl, valid, intn; } L;

void pc98_prof_port(unsigned out, unsigned port) { p_port[out][port & 0xffffu]++; }
void pc98_prof_irq(unsigned vec) { p_irq[g_cpuad.menu % 3][vec & 255u]++; }
void pc98_prof_stop(void) { p_stop[g_cpuad.menu % 3]++; }

void pc98_prof_pre(const Pc98Adapter *A)
{
    const Pc98Cpu16State *c = &A->s.low;
    uint32_t pc = (c->base[PC98_CS] + c->ip) & 0x1fffffu;
    unsigned i, n = 0, b, m = A->menu % 3;
    L.valid = 0; L.menu = m; L.has2 = 0; L.rep = 0; L.intn = 0xffff;
    L.pe = m >= 1 && (A->mode.system.msw & 1u);
    L.cpl = L.pe ? pc98_cpu_cpl16(c) : 0;
    if (pc >= 0xa0000u && pc < 0xe0000u) { p_codeunread[m]++; return; }
    for (i = 0; i < 8; i++) {
        b = mem[(pc + i) & 0x1fffffu];
        if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0xf0 || b == 0xf2 || b == 0xf3 || (b >= 0x64 && b <= 0x67)) {
            p_pfx[m][b]++;
            if (b == 0xf2 || b == 0xf3) L.rep = b;
            if (b == 0x66) p_o32[m]++;
            if (b == 0x67) p_a32[m]++;
            n++;
            continue;
        }
        break;
    }
    L.op = b;
    L.modrm = mem[(pc + i + 1) & 0x1fffffu];
    if (b == 0x0f) { L.op2 = L.modrm; L.has2 = 1; L.modrm = mem[(pc + i + 2) & 0x1fffffu]; }
    if (b == 0xcd) L.intn = mem[(pc + i + 1) & 0x1fffffu];
    L.valid = 1;
}

/* path: 0 fast16, 1 reference after fast16 refusal, 2 reference (fast16 ineligible) */
void pc98_prof_post(const Pc98Adapter *A, unsigned path)
{
    const Pc98Cpu16State *c = &A->s.low;
    unsigned m = L.menu, pe, cpl;
    p_total[m]++;
    if (L.valid) {
        if (L.has2) {
            p_0f[m][path][L.op2]++;
            p_0freg[m][L.op2][(L.modrm >> 3) & 7u]++;
        } else {
            p_main[m][path][L.op]++;
            p_mod[m][L.op][L.modrm >> 6]++;
            p_reg[m][L.op][(L.modrm >> 3) & 7u]++;
            if (L.rep && ((L.op >= 0x6c && L.op <= 0x6f) || (L.op >= 0xa4 && L.op <= 0xa7) || (L.op >= 0xaa && L.op <= 0xaf))) p_rep[m][L.op]++;
            if (L.op == 0xf4) p_hlt[m]++;
        }
        if (L.intn != 0xffffu) p_intn[m][L.intn]++;
    }
    pe = m >= 1 && (A->mode.system.msw & 1u);
    cpl = pe ? pc98_cpu_cpl16(c) : 0;
    if (L.pe) { p_pe_steps[m]++; p_pe_cpl[m][L.cpl & 3u]++; }
    if (L.pe && !pe) g_prof_evt[PEV_PE_EXIT]++;
    if (pe && L.pe && cpl != L.cpl) p_cpl_tr[L.cpl & 3u][cpl & 3u]++;
}
void pc98_prof_fault(unsigned vec, int trap)
{
    unsigned m = g_cpuad.menu % 3;
    if (trap) p_trap[m]++; else p_fault[m][vec & 31u]++;
}

void pc98_prof_reset(void)
{
    memset(g_prof_evt, 0, sizeof(g_prof_evt));
    memset(p_main, 0, sizeof(p_main)); memset(p_0f, 0, sizeof(p_0f)); memset(p_mod, 0, sizeof(p_mod));
    memset(p_reg, 0, sizeof(p_reg)); memset(p_0freg, 0, sizeof(p_0freg)); memset(p_pfx, 0, sizeof(p_pfx));
    memset(p_rep, 0, sizeof(p_rep)); memset(p_intn, 0, sizeof(p_intn)); memset(p_fault, 0, sizeof(p_fault));
    memset(p_trap, 0, sizeof(p_trap)); memset(p_irq, 0, sizeof(p_irq)); memset(p_stop, 0, sizeof(p_stop));
    memset(p_pe_steps, 0, sizeof(p_pe_steps)); memset(p_pe_cpl, 0, sizeof(p_pe_cpl)); memset(p_total, 0, sizeof(p_total));
    memset(p_cpl_tr, 0, sizeof(p_cpl_tr)); memset(p_o32, 0, sizeof(p_o32)); memset(p_a32, 0, sizeof(p_a32));
    memset(p_hlt, 0, sizeof(p_hlt)); memset(p_codeunread, 0, sizeof(p_codeunread)); memset(p_port, 0, sizeof(p_port));
    ets_printf("OK PZ\n");
}

#define P(...) ets_printf(__VA_ARGS__)
void pc98_prof_dump(void)
{
    unsigned m, p, i, j;
    P("PROFBEGIN menu=%u\n", g_cpuad.menu);
    for (m = 0; m < 3; m++) {
        if (!p_total[m]) continue;
        P("PROF,total,%u,%llu\n", m, p_total[m]);
        P("PROF,pe_steps,%u,%llu\n", m, p_pe_steps[m]);
        for (i = 0; i < 4; i++) if (p_pe_cpl[m][i]) P("PROF,pe_cpl,%u,%u,%llu\n", m, i, p_pe_cpl[m][i]);
        P("PROF,trap,%u,%llu\n", m, p_trap[m]);
        P("PROF,stop,%u,%llu\n", m, p_stop[m]);
        P("PROF,o32,%u,%llu\n", m, p_o32[m]);
        P("PROF,a32,%u,%llu\n", m, p_a32[m]);
        P("PROF,codeunread,%u,%llu\n", m, p_codeunread[m]);
        for (p = 0; p < 3; p++) for (i = 0; i < 256; i++) if (p_main[m][p][i]) P("PROF,op,%u,%u,%02x,%llu\n", m, p, i, p_main[m][p][i]);
        for (p = 0; p < 3; p++) for (i = 0; i < 256; i++) if (p_0f[m][p][i]) P("PROF,op0f,%u,%u,%02x,%llu\n", m, p, i, p_0f[m][p][i]);
        for (i = 0; i < 256; i++) for (j = 0; j < 4; j++) if (p_mod[m][i][j]) P("PROF,mod,%u,%02x,%u,%llu\n", m, i, j, p_mod[m][i][j]);
        for (i = 0; i < 256; i++) for (j = 0; j < 8; j++) if (p_reg[m][i][j]) P("PROF,reg,%u,%02x,%u,%llu\n", m, i, j, p_reg[m][i][j]);
        for (i = 0; i < 256; i++) for (j = 0; j < 8; j++) if (p_0freg[m][i][j]) P("PROF,reg0f,%u,%02x,%u,%llu\n", m, i, j, p_0freg[m][i][j]);
        for (i = 0; i < 256; i++) if (p_pfx[m][i]) P("PROF,pfx,%u,%02x,%llu\n", m, i, p_pfx[m][i]);
        for (i = 0; i < 256; i++) if (p_rep[m][i]) P("PROF,rep,%u,%02x,%llu\n", m, i, p_rep[m][i]);
        for (i = 0; i < 256; i++) if (p_intn[m][i]) P("PROF,intn,%u,%02x,%llu\n", m, i, p_intn[m][i]);
        for (i = 0; i < 32; i++) if (p_fault[m][i]) P("PROF,fault,%u,%u,%llu\n", m, i, p_fault[m][i]);
        for (i = 0; i < 256; i++) if (p_irq[m][i]) P("PROF,irq,%u,%02x,%llu\n", m, i, p_irq[m][i]);
    }
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) if (p_cpl_tr[i][j]) P("PROF,cpl_tr,%u,%u,%llu\n", i, j, p_cpl_tr[i][j]);
    for (i = 0; i < PEV_MAX; i++) if (g_prof_evt[i]) P("PROF,evt,%u,%llu\n", i, g_prof_evt[i]);
    for (p = 0; p < 2; p++) for (i = 0; i < 65536; i++) if (p_port[p][i]) P("PROF,port,%s,%04x,%u\n", p ? "w" : "r", i, (unsigned)p_port[p][i]);
    P("PROFEND\n");
}
#endif
