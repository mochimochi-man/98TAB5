/* Stage 14 diagnostic: micro-benchmarks of the HP core (build with -DPC98_HPMPROBE=1). */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern int ets_printf(const char *fmt, ...);
#define CSRR(n) ({ unsigned _v; __asm__ volatile("csrr %0, " #n : "=r"(_v)); _v; })
static volatile unsigned sink;
typedef unsigned (*fn_t)(unsigned);
static __attribute__((noinline)) unsigned f0(unsigned x) { return x + 1; }
static __attribute__((noinline)) unsigned f1(unsigned x) { return x ^ 5; }
static __attribute__((noinline)) unsigned f2(unsigned x) { return x + 7; }
static __attribute__((noinline)) unsigned f3(unsigned x) { return x * 3; }
static fn_t ftab[4] = { f0, f1, f2, f3 };
static fn_t fone[4] = { f0, f0, f0, f0 };
#define N 200000u
static __attribute__((noinline)) unsigned kBase(unsigned n) { unsigned x = 12345, s = 0; while (n--) { x = x * 1103515245u + 12345u; s += (x >> 16) & 1; } return s; }
static __attribute__((noinline)) unsigned kBrRand(unsigned n) { unsigned x = 12345, s = 0; while (n--) { x = x * 1103515245u + 12345u; if ((x >> 16) & 1) s += 3; else s ^= 5; __asm__ volatile(""); } return s; }
static __attribute__((noinline)) unsigned kBrPred(unsigned n) { unsigned x = 12345, s = 0; while (n--) { x = x * 1103515245u + 12345u; if ((x >> 16) & 0x10000u) s += 3; else s ^= 5; __asm__ volatile(""); } return s; }
static __attribute__((noinline)) unsigned kIndRand(unsigned n, fn_t *t) { unsigned x = 777, s = 0; while (n--) { x = x * 1103515245u + 12345u; s = t[(x >> 16) & 3](s); } return s; }
static __attribute__((noinline)) unsigned kLdChain(unsigned n) { static volatile uint8_t tab[256]; unsigned i, s = 0; for (i = 0; i < 256; i++) tab[i] = (uint8_t)(i * 7 + 1); while (n--) s = tab[(s + n) & 255] + s; return s; }
static __attribute__((noinline)) unsigned kLdIndep(unsigned n) { static volatile uint8_t tab[256]; unsigned i, s = 0; for (i = 0; i < 256; i++) tab[i] = (uint8_t)(i * 7 + 1); while (n--) s += tab[n & 255]; return s; }
#define T(name, expr) do { unsigned c0 = CSRR(0xB00), i0 = CSRR(0xB02); sink = (expr); unsigned c1 = CSRR(0xB00), i1 = CSRR(0xB02); \
    ets_printf("HPM %s cyc/iter=%u.%02u ins/iter=%u.%02u\n", name, (c1 - c0) / N, ((c1 - c0) * 100 / N) % 100, (i1 - i0) / N, ((i1 - i0) * 100 / N) % 100); } while (0)
static void probe_once(void)
{
    ets_printf("HPMPROBE start\n");
    T("base(no branch)", kBase(N));
    T("branch random", kBrRand(N));
    T("branch predictable", kBrPred(N));
    T("indirect random4", kIndRand(N, ftab));
    T("indirect same", kIndRand(N, fone));
    T("ld chain", kLdChain(N));
    T("ld indep", kLdIndep(N));
    ets_printf("HPMPROBE done\n");
}
void hpm_probe(void) { for (;;) { vTaskDelay(pdMS_TO_TICKS(6000)); probe_once(); } }
