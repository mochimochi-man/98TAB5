/* SPDX-License-Identifier: MIT
 * ATAPI CD-ROM drive (dev/atapi.c + media/cdimg.c) driven the way CD98.SYS drives it.
 *   cc -I../../components/pc98core/include -o atapi_test atapi_test.c && ./atapi_test DISC.ISO */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hostfile.h>

struct HostFileTag { FILE *f; };
HFILE hf_open(const char *p) { FILE *f = fopen(p, "rb"); HFILE h; if (!f) return NULL; h = malloc(sizeof(*h)); h->f = f; return h; }
long hf_seek(HFILE h, long pos, int w) { fseek(h->f, pos, w == HF_SEEK_SET ? SEEK_SET : w == HF_SEEK_CUR ? SEEK_CUR : SEEK_END); return ftell(h->f); }
UINT hf_read(HFILE h, void *d, UINT n) { return (UINT)fread(d, 1, n, h->f); }
short hf_close(HFILE h) { fclose(h->f); free(h); return 0; }
long hf_size(HFILE h) { long c = ftell(h->f), n; fseek(h->f, 0, SEEK_END); n = ftell(h->f); fseek(h->f, c, SEEK_SET); return n; }

void host_cd_access(void) {}

#include "../../components/pc98core/media/cdimg.c"
#include "../../components/pc98core/dev/atapi.c"

static int irqs;
static void on_irq(void) { irqs++; }

static uint8_t st(void) { return atapi_in(0x64e); }

/* Send a packet; read the reply into buf (up to cap). Returns bytes, or -1 on error (sense key in *sk). */
static int send_packet(const uint8_t *p, uint8_t *buf, int cap, int *sk)
{
	int n = 0, i;

	atapi_out(0x642, 0);
	atapi_out(0x648, 0x00);
	atapi_out(0x64a, 0x80);
	atapi_out(0x64e, 0xa0);
	assert(!(st() & 0x80) && (st() & 0x08) && atapi_in(0x644) == 1);
	for (i = 0; i < 12; i++) atapi_out(i & 1 ? 0x641 : 0x640, p[i]);
	while (st() & 0x08) {
		int cnt = atapi_in(0x648) | (atapi_in(0x64a) << 8);

		assert(atapi_in(0x644) == 2);
		for (i = 0; i < cnt; i++) {
			uint8_t v = atapi_in(i & 1 ? 0x641 : 0x640);
			if (n < cap) buf[n] = v;
			n++;
		}
	}
	assert(atapi_in(0x644) == 3);
	if (st() & 0x01) { *sk = atapi_in(0x642) >> 4; return -1; }
	return n;
}

int main(int argc, char **argv)
{
	static uint8_t buf[65536], ref[65536];
	uint8_t p[12];
	int sk = 0, n, i;
	FILE *f;

	assert(argc > 1);
	atapi_irq_hook = on_irq;
	atapi_fit(1);
	assert(atapi_insert(argv[1]) == 0);
	atapi_reset();
	assert(atapi_in(0x648) == 0x14 && atapi_in(0x64a) == 0xeb);

	/* IDENTIFY PACKET DEVICE */
	atapi_out(0x64c, 0xa0);
	atapi_out(0x64e, 0xa1);
	assert(st() & 0x08);
	for (i = 0; i < 512; i++) buf[i] = atapi_in(i & 1 ? 0x641 : 0x640);
	assert((buf[1] & 0xdf) == 0x85);
	assert(!(st() & 0x08));

	/* a slave is absent */
	atapi_out(0x64c, 0xb0);
	assert(atapi_in(0x64e) == 0x00);
	atapi_out(0x64c, 0xa0);

	memset(p, 0, 12);				/* TEST UNIT READY: unit attention first */
	assert(send_packet(p, buf, sizeof(buf), &sk) < 0 && sk == 6);
	assert(send_packet(p, buf, sizeof(buf), &sk) == 0);

	memset(p, 0, 12); p[0] = 0x25;			/* READ CAPACITY */
	assert(send_packet(p, buf, sizeof(buf), &sk) == 8);
	n = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
	printf("capacity %d sectors\n", n + 1);

	memset(p, 0, 12); p[0] = 0x28; p[5] = 16; p[8] = 20;	/* READ(10) LBA 16, 20 sectors */
	assert(send_packet(p, buf, sizeof(buf), &sk) == 20 * 2048);
	f = fopen(argc > 2 ? argv[2] : argv[1], "rb");
	fseek(f, 16 * 2048, SEEK_SET);
	assert(fread(ref, 1, 20 * 2048, f) == 20 * 2048);
	fclose(f);
	assert(memcmp(buf, ref, 20 * 2048) == 0);
	assert(memcmp(buf + 1, "CD001", 5) == 0);

	memset(p, 0, 12); p[0] = 0x28; p[2] = 0x7f; p[8] = 1;	/* out of range */
	assert(send_packet(p, buf, sizeof(buf), &sk) < 0 && sk == 5);
	memset(p, 0, 12); p[0] = 0x03; p[4] = 18;	/* REQUEST SENSE */
	assert(send_packet(p, buf, sizeof(buf), &sk) == 18 && buf[2] == 5 && buf[12] == 0x21);

	memset(p, 0, 12); p[0] = 0x43; p[1] = 2; p[7] = 3; p[8] = 0x20;	/* READ TOC, MSF */
	n = send_packet(p, buf, sizeof(buf), &sk);
	assert(n == 4 + (buf[3] - buf[2] + 2) * 8 && buf[2] == 1);
	printf("TOC: %d track(s), second entry control %02x\n", buf[3], buf[13]);
	assert(buf[4 + 2] == 1 && buf[4 + 5] == 0 && buf[4 + 6] == 2 && buf[4 + 7] == 0);	/* track 1 at 00:02:00 */
	assert(buf[n - 8 + 2] == 0xaa);

	atapi_insert(NULL);				/* eject: not ready */
	memset(p, 0, 12);
	assert(send_packet(p, buf, sizeof(buf), &sk) < 0 && sk == 2);
	printf("atapi_test: ok (%d interrupts)\n", irqs);
	return 0;
}
