# SPDX-License-Identifier: MIT
# Copyright (c) mochimochi-man / Uh
#
# gen_clock_tables.py - writes clock_tables.inc and clock_tables386.inc: the CPU clocks of every instruction form,
# from the manufacturers' manuals (Intel 80286 Programmer's Reference Manual, chapter 8; NEC V20/V30 User's Manual,
# instruction set chapter; Intel 80386 Programmer's Reference Manual, chapter 17).
#   index of the main / 0F tables: (opcode << 4) | (ModR/M reg field << 1) | (1 = memory operand)
# Where the manual gives a range (data dependent) the upper value is taken.  "+m" (the 286 / 386 charge per byte of the
# next instruction after a control transfer) is counted as 2.  The V30 values are for word operands at even addresses.
# Usage: python gen_clock_tables.py   (in this directory)

M = 2           # "+m"

def alu_group(v30):
	# ADD OR ADC SBB AND SUB XOR CMP: r/m,reg  reg,r/m  acc,imm
	if v30:
		return dict(rm_r=(2, 16), r_rm=(2, 11), acc=4, cmp_rm_r=(2, 11))
	return dict(rm_r=(2, 7), r_rm=(2, 7), acc=3, cmp_rm_r=(2, 7), cmp_r_rm=(2, 6))

def main_cost(v30, op, reg, mem):
	r = 0 if not mem else 1
	def pick(pair): return pair[r]
	if op < 0x40 and (op & 7) < 6:
		k, f = op >> 3, op & 7
		cmp = (k == 7)
		if f in (4, 5): return 4 if v30 else 3
		if v30:
			if f in (0, 1): return pick((2, 11 if cmp else 16))
			return pick((2, 11))
		if f in (0, 1): return pick((2, 7))
		return pick((2, 6 if cmp else 7))
	if op in (0x06, 0x0e, 0x16, 0x1e): return 8 if v30 else 3              # PUSH seg
	if op in (0x07, 0x17, 0x1f): return 8 if v30 else 5                    # POP seg
	if op in (0x26, 0x2e, 0x36, 0x3e, 0xf0, 0xf2, 0xf3): return 2 if v30 else 0   # prefixes
	if op in (0x27, 0x37): return 3                                        # DAA AAA
	if op in (0x2f, 0x3f): return 7 if v30 else 3                          # DAS AAS
	if 0x40 <= op <= 0x4f: return 2                                        # INC / DEC reg
	if 0x50 <= op <= 0x57: return 8 if v30 else 3                          # PUSH reg
	if 0x58 <= op <= 0x5f: return 8 if v30 else 5                          # POP reg
	if op == 0x60: return 35 if v30 else 17                                # PUSHA
	if op == 0x61: return 43 if v30 else 19                                # POPA
	if op == 0x62: return 18 if v30 else 13                                # BOUND
	if op in (0x68, 0x6a): return 7 if v30 else 3                          # PUSH imm
	if op in (0x69, 0x6b): return pick((36, 42) if v30 else (21, 24))      # IMUL imm
	if op in (0x6c, 0x6d): return 10 if v30 else 5                         # INS
	if op in (0x6e, 0x6f): return 10 if v30 else 5                         # OUTS
	if 0x70 <= op <= 0x7f: return 4 if v30 else 3                          # Jcc not taken (taken: +taken table)
	if op in (0x80, 0x81, 0x82, 0x83):                                     # group 1 immediate
		if reg == 7: return pick((4, 13) if v30 else (3, 6))
		return pick((4, 18) if v30 else (3, 7))
	if op in (0x84, 0x85): return pick((2, 10) if v30 else (2, 6))         # TEST
	if op in (0x86, 0x87): return pick((3, 16) if v30 else (3, 5))         # XCHG
	if op in (0x88, 0x89): return pick((2, 9) if v30 else (2, 3))          # MOV r/m,reg
	if op in (0x8a, 0x8b): return pick((2, 11) if v30 else (2, 5))         # MOV reg,r/m
	if op == 0x8c: return pick((2, 10) if v30 else (2, 3))                 # MOV r/m,seg
	if op == 0x8d: return 4 if v30 else 3                                  # LEA
	if op == 0x8e: return pick((2, 11) if v30 else (2, 5))                 # MOV seg,r/m
	if op == 0x8f: return pick((8, 17) if v30 else (5, 5))                 # POP r/m
	if op == 0x90 or 0x91 <= op <= 0x97: return 3                          # NOP, XCHG AX,reg
	if op == 0x98: return 2                                                # CBW
	if op == 0x99: return 5 if v30 else 2                                  # CWD
	if op == 0x9a: return 21 if v30 else 13 + M                            # CALL far
	if op == 0x9b: return 2 if v30 else 3                                  # WAIT
	if op == 0x9c: return 8 if v30 else 3                                  # PUSHF
	if op == 0x9d: return 8 if v30 else 5                                  # POPF
	if op == 0x9e: return 3 if v30 else 2                                  # SAHF
	if op == 0x9f: return 2                                                # LAHF
	if op in (0xa0, 0xa1): return 10 if v30 else 5                         # MOV acc,mem
	if op in (0xa2, 0xa3): return 9 if v30 else 3                          # MOV mem,acc
	if op in (0xa4, 0xa5): return 11 if v30 else 5                         # MOVS
	if op in (0xa6, 0xa7): return 13 if v30 else 8                         # CMPS
	if op in (0xa8, 0xa9): return 4 if v30 else 3                          # TEST acc,imm
	if op in (0xaa, 0xab): return 7 if v30 else 3                          # STOS
	if op in (0xac, 0xad): return 7 if v30 else 5                          # LODS
	if op in (0xae, 0xaf): return 10 if v30 else 7                         # SCAS
	if 0xb0 <= op <= 0xbf: return 4 if v30 else 2                          # MOV reg,imm
	if op in (0xc0, 0xc1, 0xd2, 0xd3): return pick((7, 19) if v30 else (5, 8))   # shift by count (+ per bit)
	if op in (0xd0, 0xd1): return pick((2, 16) if v30 else (2, 7))         # shift by 1
	if op == 0xc2: return 20 if v30 else 11 + M                            # RET near imm
	if op == 0xc3: return 15 if v30 else 11 + M                            # RET near
	if op in (0xc4, 0xc5): return 18 if v30 else 7                         # LES LDS
	if op in (0xc6, 0xc7): return pick((4, 11) if v30 else (2, 3))         # MOV r/m,imm
	if op == 0xc8: return 16 if v30 else 11                                # ENTER (level 0)
	if op == 0xc9: return 6 if v30 else 5                                  # LEAVE
	if op == 0xca: return 24 if v30 else 15 + M                            # RET far imm
	if op == 0xcb: return 21 if v30 else 15 + M                            # RET far
	if op == 0xcc: return 50 if v30 else 23 + M                            # INT 3
	if op == 0xcd: return 50 if v30 else 23 + M                            # INT n
	if op == 0xce: return 3                                                # INTO (not taken)
	if op == 0xcf: return 27 if v30 else 17 + M                            # IRET
	if op == 0xd4: return 15 if v30 else 16                                # AAM
	if op == 0xd5: return 7 if v30 else 14                                 # AAD
	if op == 0xd6: return 3                                                # (SALC on the V30 / undefined)
	if op == 0xd7: return 9 if v30 else 5                                  # XLAT
	if 0xd8 <= op <= 0xdf: return pick((2, 11) if v30 else (9, 9))         # ESC
	if op in (0xe0, 0xe1): return 5 if v30 else 4                          # LOOPNZ / LOOPZ not taken
	if op == 0xe2: return 5 if v30 else 4                                  # LOOP not taken
	if op == 0xe3: return 5 if v30 else 4                                  # JCXZ not taken
	if op in (0xe4, 0xe5): return 9 if v30 else 5                          # IN imm
	if op in (0xe6, 0xe7): return 8 if v30 else 3                          # OUT imm
	if op == 0xe8: return 16 if v30 else 7 + M                             # CALL near
	if op == 0xe9: return 13 if v30 else 7 + M                             # JMP near
	if op == 0xea: return 15 if v30 else 11 + M                            # JMP far
	if op == 0xeb: return 12 if v30 else 7 + M                             # JMP short
	if op in (0xec, 0xed): return 8 if v30 else 5                          # IN DX
	if op in (0xee, 0xef): return 8 if v30 else 3                          # OUT DX
	if op == 0xf4: return 2                                                # HLT
	if op == 0xf5: return 2                                                # CMC
	if op in (0xf6, 0xf7):                                                 # group 3
		w = op & 1
		if reg in (0, 1): return pick((4, 11) if v30 else (3, 6))          # TEST imm
		if reg in (2, 3): return pick((2, 16) if v30 else (2, 7))          # NOT NEG
		if reg == 4: return pick(((30, 36) if w else (22, 28)) if v30 else ((21, 24) if w else (13, 16)))   # MUL
		if reg == 5: return pick(((47, 53) if w else (39, 45)) if v30 else ((21, 24) if w else (13, 16)))   # IMUL
		if reg == 6: return pick(((25, 31) if w else (19, 25)) if v30 else ((22, 25) if w else (14, 17)))   # DIV
		return pick(((43, 49) if w else (34, 40)) if v30 else ((25, 28) if w else (17, 20)))               # IDIV
	if op in (0xf8, 0xf9, 0xfc, 0xfd): return 2                            # CLC STC CLD STD
	if op == 0xfa: return 2 if v30 else 3                                  # CLI
	if op == 0xfb: return 2                                                # STI
	if op == 0xfe:                                                         # INC / DEC r/m8
		return pick((2, 16) if v30 else (2, 7))
	if op == 0xff:
		if reg in (0, 1): return pick((2, 16) if v30 else (2, 7))          # INC DEC
		if reg == 2: return pick((14, 23) if v30 else (7 + M, 11 + M))     # CALL near indirect
		if reg == 3: return pick((31, 31) if v30 else (16 + M, 16 + M))    # CALL far indirect
		if reg == 4: return pick((11, 20) if v30 else (7 + M, 11 + M))     # JMP near indirect
		if reg == 5: return pick((27, 27) if v30 else (15 + M, 15 + M))    # JMP far indirect
		if reg == 6: return pick((8, 18) if v30 else (3, 5))               # PUSH r/m
		return 20
	if op == 0x0f: return 0
	return 20                                                              # not an instruction: the fault path

def taken(v30, op):
	# the extra clocks of a taken branch
	if 0x70 <= op <= 0x7f: return (14 - 4) if v30 else (7 + M - 3)
	if op in (0xe0, 0xe1): return (14 - 5) if v30 else (8 + M - 4)
	if op == 0xe2: return (13 - 5) if v30 else (8 + M - 4)
	if op == 0xe3: return (13 - 5) if v30 else (8 + M - 4)
	return 0

def shift_bit(v30): return 1     # clocks per bit of the count (both manuals: n)

def rep(v30, op):
	# (clocks with CX = 0, clocks per iteration)
	t286 = {0xa4: (5, 4), 0xa5: (5, 4), 0xa6: (5, 9), 0xa7: (5, 9), 0xaa: (4, 3), 0xab: (4, 3),
		0xac: (5, 4), 0xad: (5, 4), 0xae: (5, 8), 0xaf: (5, 8), 0x6c: (5, 4), 0x6d: (5, 4), 0x6e: (5, 4), 0x6f: (5, 4)}
	tv30 = {0xa4: (11, 8), 0xa5: (11, 8), 0xa6: (7, 14), 0xa7: (7, 14), 0xaa: (7, 4), 0xab: (7, 4),
		0xac: (7, 9), 0xad: (7, 9), 0xae: (7, 10), 0xaf: (7, 10), 0x6c: (9, 8), 0x6d: (9, 8), 0x6e: (9, 8), 0x6f: (9, 8)}
	return (tv30 if v30 else t286).get(op, (0, 0))

def ext_cost(v30, op2, reg, mem):
	r = 1 if mem else 0
	if v30:
		# NEC extended instructions (TEST1 / CLR1 / SET1 / NOT1, INS / EXT, ADD4S / SUB4S / CMP4S, ROL4 / ROR4)
		if 0x10 <= op2 <= 0x1f: return (4, 12)[r] if op2 & 8 else (3, 12)[r]
		if op2 in (0x20, 0x22, 0x26): return 22
		if op2 in (0x28, 0x2a): return (25, 28)[r]
		if op2 in (0x31, 0x33, 0x39, 0x3b): return 75
		if op2 == 0xff: return 50     # BRKEM
		return 20
	# 80286 system instructions (real-mode figures)
	if op2 == 0x00: return (2, 3)[r] if reg in (0, 1) else (17, 19)[r]    # SLDT STR / LLDT LTR VERR VERW
	if op2 == 0x01:
		if reg in (0, 1): return 11                                        # SGDT SIDT
		if reg in (2, 3): return 11                                        # LGDT LIDT
		if reg == 4: return (2, 3)[r]                                      # SMSW
		if reg == 6: return (3, 6)[r]                                      # LMSW
		return 20
	if op2 in (0x02, 0x03): return 14 if op2 == 2 else 14                  # LAR LSL
	if op2 == 0x05: return 195                                             # LOADALL
	if op2 == 0x06: return 2                                               # CLTS
	return 20

def table(fn, v30):
	return [fn(v30, op, reg, mem) for op in range(256) for reg in range(8) for mem in range(2)]

def emit(name, vals, f):
	f.write('{\n')
	for i in range(0, len(vals), 32):
		f.write(','.join(str(min(255, v)) for v in vals[i:i + 32]) + ',\n')
	f.write('},\n')

with open('clock_tables.inc', 'w', newline='\n') as f:
	f.write('/* SPDX-License-Identifier: MIT\n * Copyright (c) mochimochi-man / Uh\n'
		' * GENERATED by gen_clock_tables.py from the manufacturers\' manuals. Do not edit.\n'
		' * [0] NEC V30, [1] Intel 80286; main / 0F index (opcode << 4) | (reg << 1) | memory operand */\n')
	f.write('static const uint8_t clock_main[2][4096] = {\n')
	for v in (1, 0): emit('main', table(main_cost, v), f)
	f.write('};\nstatic const uint8_t clock_0f[2][4096] = {\n')
	for v in (1, 0): emit('0f', table(ext_cost, v), f)
	f.write('};\nstatic const uint8_t clock_taken[2][256] = {\n')
	for v in (1, 0): emit('t', [taken(v, op) for op in range(256)], f)
	f.write('};\n')
	f.write('static const uint8_t clock_shift[2][4] = {{1,1,1,1},{1,1,1,1}};\n')
	f.write('static const uint8_t clock_rep_zero[2][256] = {\n')
	for v in (1, 0): emit('rz', [rep(v, op)[0] for op in range(256)], f)
	f.write('};\nstatic const uint8_t clock_rep_iter[2][256] = {\n')
	for v in (1, 0): emit('ri', [rep(v, op)[1] for op in range(256)], f)
	f.write('};\nstatic const uint8_t clock_interrupt[2] = {50, %d};\n' % (23 + M))

# ---- 80386 (the extension menu): 32-bit operand forms and the 0F map ----

def c386_main(_, op, reg, mem):
	r = 1 if mem else 0
	if op < 0x40 and (op & 7) < 6:
		f = op & 7
		if f in (4, 5): return 2
		cmp = (op >> 3) == 7
		return (2, 5 if cmp else 7)[r] if f in (0, 1) else (2, 6)[r]
	if 0x40 <= op <= 0x4f: return 2
	if 0x50 <= op <= 0x57: return 2
	if 0x58 <= op <= 0x5f: return 4
	if op == 0x60: return 18
	if op == 0x61: return 24
	if op in (0x68, 0x6a): return 2
	if op in (0x69, 0x6b): return (38, 41)[r]
	if op in (0x80, 0x81, 0x83): return (2, 5 if reg == 7 else 7)[r]
	if op in (0x84, 0x85): return (2, 5)[r]
	if op in (0x86, 0x87): return (3, 5)[r]
	if op in (0x88, 0x89): return (2, 2)[r]
	if op in (0x8a, 0x8b): return (2, 4)[r]
	if op == 0x8c: return (2, 2)[r]
	if op == 0x8d: return 2
	if op == 0x8e: return (2, 5)[r]
	if op == 0x8f: return 5
	if 0x90 <= op <= 0x97: return 3
	if op in (0x98, 0x99): return 3 if op == 0x98 else 2
	if op == 0x9c: return 4
	if op == 0x9d: return 5
	if op in (0xa0, 0xa1): return 4
	if op in (0xa2, 0xa3): return 2
	if op in (0xa4, 0xa5): return 7
	if op in (0xa6, 0xa7): return 10
	if op in (0xa8, 0xa9): return 2
	if op in (0xaa, 0xab): return 4
	if op in (0xac, 0xad): return 5
	if op in (0xae, 0xaf): return 7
	if 0xb8 <= op <= 0xbf: return 2
	if op in (0xc1, 0xd3): return (3, 7)[r]
	if op == 0xd1: return (3, 7)[r]
	if op == 0xc7: return (2, 2)[r]
	if op in (0xe5, 0xed): return 12
	if op in (0xe7, 0xef): return 10
	if op == 0xf7:
		if reg in (0, 1): return (2, 5)[r]
		if reg in (2, 3): return (2, 6)[r]
		if reg in (4, 5): return (38, 41)[r]
		if reg == 6: return (38, 41)[r]
		return (43, 46)[r]
	if op == 0xff:
		if reg in (0, 1): return (2, 6)[r]
		if reg == 6: return (2, 5)[r]
		return (7 + M, 10 + M)[r]
	return main_cost(0, op, reg, mem)

def c386_0f(o32, op, reg, mem):
	r = 1 if mem else 0
	if 0x80 <= op <= 0x8f: return 3                     # Jcc near not taken
	if 0x90 <= op <= 0x9f: return (4, 5)[r]             # SETcc
	if op in (0xa0, 0xa8): return 2                     # PUSH FS / GS
	if op in (0xa1, 0xa9): return 7                     # POP FS / GS
	if op == 0xa3: return (3, 12)[r]                    # BT
	if op in (0xab, 0xb3, 0xbb): return (6, 13)[r]      # BTS BTR BTC
	if op == 0xba: return (3, 6)[r] if reg == 4 else (6, 8)[r]
	if op in (0xa4, 0xac): return (3, 7)[r]             # SHLD SHRD imm
	if op in (0xa5, 0xad): return (3, 7)[r]             # SHLD SHRD CL
	if op == 0xaf: return (38, 41)[r]                   # IMUL r, r/m
	if op in (0xb2, 0xb4, 0xb5): return 7               # LSS LFS LGS
	if op in (0xb6, 0xb7, 0xbe, 0xbf): return (3, 6)[r] # MOVZX MOVSX
	if op in (0xbc, 0xbd): return (42, 45)[r]           # BSF BSR (upper)
	return ext_cost(0, op, reg, mem)

with open('clock_tables386.inc', 'w', newline='\n') as f:
	f.write('/* SPDX-License-Identifier: MIT\n * Copyright (c) mochimochi-man / Uh\n'
		' * GENERATED by gen_clock_tables.py from the Intel 80386 Programmer\'s Reference Manual. Do not edit.\n'
		' * Index: (opcode << 4) | (reg << 1) | memory operand */\n')
	def emit1(name, vals):
		f.write('static const uint8_t %s[%d] = {\n' % (name, len(vals)))
		for i in range(0, len(vals), 32):
			f.write(','.join(str(min(255, v)) for v in vals[i:i + 32]) + ',\n')
		f.write('};\n')
	emit1('clock_386_main66', table(c386_main, 1))
	emit1('clock_386_0f', table(c386_0f, 0))
	emit1('clock_386_0f66', table(c386_0f, 1))
	f.write('static const uint8_t clock_386_jcc_taken = %d;\n' % (7 + M - 3))
	emit1('clock_386_rep66_zero', [rep(0, op)[0] for op in range(256)])
	emit1('clock_386_rep66_iter', [rep(0, op)[1] for op in range(256)])
