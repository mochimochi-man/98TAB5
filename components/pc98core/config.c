/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * config.c - the configuration's defaults and the machine description a reset derives from it (pc98.h). */
#include <compiler.h>
#include <pc98.h>
#include <cpulink.h>

Pc98Settings g_settings;
MachineDesc g_mach = {
	.base_clock	= CLK_BASE5,
	.clock_mul	= 4,
	.cpu_clock	= CLK_BASE5 * 4,
	.model		= MODEL_VX,
	.dip_switch	= { 0x3e, 0xe3, 0x7b },
};

/* a PC-9801VX with the factory switch settings, a 26K board and two floppy drives */
static const Pc98Settings s_default = {
	.draw_at_retrace	= 1,
	.has_16color		= 1,
	.gfx_chip			= 3,
	.key_repeat	= 1,
	.key_repeat_delay_ms	= 500,
	.key_repeat_rate_ms	= 60,
	.model			= "VX",
	.base_clock		= CLK_BASE5,
	.clock_mul		= 4,
	.dip_switch			= { 0x3e, 0xe3, 0x7b },
	.mem_switch			= { 0x48, 0x05, 0x04, 0x08, 0x01, 0x00, 0x00, 0x6e },
	.ext_mb			= 13,
	.vram_wait			= { 1, 1, 4, 1, 6, 1 },
	.sample_rate		= 22050,
	.audio_block_ms		= 20,
	.sound_board		= SOUND_26K,
	.board26k_jumpers		= 0xd1,
	.beep_level		= 2,
	.volume_all		= 70,
	.volume_fm			= 100,
	.volume_ssg		= 100,
	.fdd_fitted		= 3,
	.fdd_144		= 1,
};

void config_defaults(Pc98Settings *cfg)
{
	*cfg = s_default;
}

static int same_text(const char *s, const char *t)
{
	for (;; s++, t++) {
		int a = (UINT8)*s, b = (UINT8)*t;
		if (a >= 'a' && a <= 'z') a -= 0x20;
		if (a != b) return 0;
		if (!a) return 1;
	}
}

void config_derive(MachineDesc *m, const Pc98Settings *cfg)
{
	UINT mul = cfg->clock_mul;

	memset(m, 0, sizeof(*m));
	m->model = same_text(cfg->model, "VM") ? MODEL_VM : MODEL_VX;
	m->clk8mhz = cfg->base_clock < (CLK_BASE5 + CLK_BASE8) / 2;
	m->base_clock = m->clk8mhz ? CLK_BASE8 : CLK_BASE5;
	if (mul < 1) mul = 1;
	if (mul > CLOCK_MUL_MAX) mul = CLOCK_MUL_MAX;
	m->clock_mul = mul;
	m->cpu_clock = m->base_clock * mul;
	m->ext_mb = (cfg->dip_switch[2] & 0x80) ? 0 : (cfg->ext_mb > 13 ? 13 : cfg->ext_mb);
	memcpy(m->dip_switch, cfg->dip_switch, 3);
	m->dip_switch[0] &= (UINT8)~1u;	/* DIP switch 1-1 on: the 24 kHz monitor, the only one emulated */
	m->sound = (SoundBoard)cfg->sound_board;
}
