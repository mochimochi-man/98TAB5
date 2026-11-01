# Host CPU timing profiles

This module is an opt-in host subset. Firmware still uses the legacy CPU.

The manufacturer profile records documented instruction clocks independently
of the observed PC-98 software compatibility profile. Bus wait states,
prefetch overlap, exception delivery and DMA are not certified here.

| Prefix-free encoding | Intel 80286 | NEC V30 | PC-98 compatibility |
|---|---:|---:|---:|
| 40..4F short INC/DEC | 2 | 2 | 2 |
| 90..97 NOP/AX XCHG | 3 | 3 | 3 |
| 98 CBW | 2 | 2 | 2 |
| 99 CWD | 2 | 4..5 | 2 |
| B0..BF immediate MOV | 2 | 4 | 2 |
| 04/05..3C/3D accumulator ALU immediate | 3 | 4 | 3 |
| A8/A9 accumulator TEST immediate | 3 | 4 | 3 |
| FA CLI | 3 | 2 | 2 |
| F5/F8/F9/FB/FC/FD flag controls | 2 | 2 | 2 |
| F4 HLT nominal minimum | 2 | 2 | slice idle |

Sources are the [Intel 80286 Programmer Reference Manual (1987)](https://bitsavers.org/components/intel/80286/210498-005_80286_and_80287_Programmers_Reference_Manual_1987.pdf),
instruction entries in chapter 8, and the [NEC V20/V30 User Manual](https://www.dosdays.co.uk/media/nec/NEC-V20-V30-Users-Manual.pdf),
MOV reg,imm (12-5), INC reg16 (12-48), DEC reg16, XCH AW,reg16,
CVTBW/CVTWL (12-64) and processor-control entries.

NEC specifies a data-dependent range for CWD without an exact value mapping
in that entry. The metadata retains both bounds. Manufacturer execution stops
before executing this opcode rather than selecting an unverified bound.
Compatibility CWD is backed by unchanged legacy observations for every AX
value in all three persisted menus, with CPU_TYPE explicitly reselected after
RESET. Immediate MOV compatibility differs from NEC documentation as well.
Menu 2 uses the 286 native profile for these prefix-free instructions.

HLT positive remaining time becomes -1 using scheduler-compatible idle
accounting, absorbing its minimum without a second debit. A committed TF trap
charges the instruction once and stops before handler delivery or idle.
Unknown instructions and prefixes stop before execution. They are not charged
zero and continued. Faults do not receive an estimated delivery cost.

Accumulator ADD/ADC/SUB/SBB/CMP/AND/OR/XOR and TEST immediate forms use
the corresponding manufacturer instruction entries. Legacy public observations
cover all 18 encodings in all three menus with both source sign samples
(108 observations), explicitly restoring CPU_TYPE after RESET. All show 3 clocks.

Short Jcc (70..7F) is a separate FLAGS-dependent family, not a fixed-clock
metadata entry. NEC conditional branches (12-157 through 12-164) specify
14 clocks taken and 4 otherwise. Intel286 Jcond (8-54) gives 7/3 and explicitly
adds one clock per byte of the next executed instruction when taken.
Manufacturer mode therefore stops before taken 286/hybrid Jcc until that
next-instruction dependency can be accounted for without speculative fetch.
Not-taken Intel3 and NEC14/4 are exact at this level. Legacy compatibility
uses7/3, confirmed with all32 relevant FLAGS combinations across all16 Jcc
and all3 persisted menus (1536 observations with explicit CPU_TYPE selection).
Zero displacement remains a taken branch if its condition is true.

Register ModR/M forms: ALU00..03 through38..3B, MOV88..8B, TEST84/85
and XCHG86/87. Manufacturer register costs are2 except XCHG3 for both286
and V30. Group1 register immediates80/81/83 cost2863, NEC4 and legacy3.
See each manufacturer's ALU/MOV/TEST/XCHG instruction entries; immediate
sign extension does not change the documented register cost. Legacy public
ABI16512 observations cover all64 register aliases, both source samples
and all3 menus with independent clock/IP assertions.

This classifier preflights and fetches opcode/ModR/M once and replays both
bytes to the existing executor. It never prefetches a displacement, immediate
or data operand for an unknown memory timing form. Missing second code byte
raises its normal CS fault before a physical read and before execution.
No memory EA, alignment, physical LOCK or bus wait timing is inferred.

Memory MOV88..8B uses Intel286 load5/store3, also observed unchanged legacy
compatibility clocks in4608 native memory ModR/M samples across all menus.
NEC tables MOV mem,reg and MOV reg,mem (12-4 through12-5) distinguish store9
and load11 for byte or aligned V30 word transfers; odd word transfers add4.
Native V30 manufacturer word timing computes parity from input BX/BP/SI/DI,
the default DS/SS base and (when present) the displacement low byte. No data
read or high displacement byte is required for parity. That code byte is
preflighted and replayed once, up to three cached bytes. Existing execution
owns full EA/width/rights checks; faults never charge nominal MOV clocks.
Other memory timings, wait states and physical bus microtiming remain pending.

Memory ALU00..03 through38..3B and TEST84/85 are timed separately from MOV.
Intel286 ALU costs7, CMP memory-destination7/register-destination6, and
TEST6. Legacy CMP uses6 in both directions. NEC ALU memory source costs11,
word odd15; memory destination RMW costs16, word odd24. CMP is a read-only
source form in both directions,11/15. NEC memory TEST is10/14, not11/15.
These values follow each manufacturer's instruction entry; legacy39168
observations cover all34 memory opcode forms and all native ModR/M encodings
with independent clock/IP assertions. Native manufacturer alignment uses the
same parity-only predecode. Existing execution retains READ versus WRITE
permission checks, full-width preflight and transaction order; a denied RMW
receives no instruction debit and touches no operand data.

Group1 memory immediates80/81/83: Intel286 RMW7 and CMP6; legacy uses7
for every suboperation, including CMP (3456 clock/IP observations across all
EA forms, widths, operation groups and menus). NEC RMW18/oddword26 and
CMP13/oddword17. Immediate width/sign extension changes decoding, not the
memory transfer timing for these forms. Six-byte CS truncation and read-only
CPL3 protections remain delegated to the normal executor and never receive an
estimated fault-delivery debit.

Correction after the CPU113 checkpoint: Intel286 CMP table8-33 distinguishes
memory-destination CMP38/39 at7 from register-destination CMP3A/3B at6.
CPU113 incorrectly used the legacy6 for both manufacturer directions.
Current execution and independent timing tests now retain that distinction.
The same table specifies immediate memory CMP6, while legacy observation is7.
Historical snapshots are preserved without retroactive edits.

The context-aware quantum API additionally supports literal F3 A4 (REP MOVSB)
and F3 AA (REP STOSB). Intel286 REP table8-92 gives MOVSB setup5 plus4 per
iteration, STOSB setup4 plus3 per iteration; CX0 pays setup only. One quantum
executes at most one existing-backend iteration. Partial iterations are charged
without falsely retiring the instruction, releasing shadows or clearing hybrid
RF. Data/fetch faults retain preceding iterations and clocks without estimating
fault-delivery latency. Multi-iteration TF remains unsupported.

A caller-owned per-CPU repeat timing context retains setup across manufacturer
slices. Invalidate it on accepted interrupt, context switch/debugger edit or
explicit restart; a blocked IRQ retains it. The compatibility runner restarts
setup on each legacy EXEC entry, as confirmed by960 unchanged legacy public
observations including actual re-entry slices. V30 uses this observed software
compatibility profile only; NEC manufacturer REP timing remains unsupported.
Zero-initialize the context and include its explicit lifecycle in a future
machine/save-state adapter. Clock/event/DMA ownership is not transferred.

Stateless fixed APIs retain their previous scope. Context-aware slice quota
counts quanta, including partial REP iterations; retired counts whole
instructions only. Tests exercise actual scheduler events at5 and20 clocks,
partial REP overshoot, both setup policies, accepted/blocked IRQ with explicit
untimed IRET, and precise protected operand faults after committed iterations.
This is not yet a firmware IRQ/DMA loop or physical prefetch timing proof.
