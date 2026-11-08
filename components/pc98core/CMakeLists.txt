# SPDX-License-Identifier: MIT
# pc98core - the PC-9801 machine (mochimochi-man / Uh).
#   machine.c config.c bus.c io.c events.c   the machine, its configuration, the memory and I/O buses, the event scheduler
#   cpu/        the CPU (V30 / 80286 / 80286 with 386 extensions) and its adapter cpuadapt.c
#   dev/        interrupt controller, timer, PPI, calendar, keyboard, mouse, DMA, floppy / SASI / IDE, CG window, system ports
#   video/      GDCs, GRCG, EGC, VRAM access, CG ROM and the renderer
#   sound/      the mixer, the beeper and the PC-9801-26K (YM2203)
#   media/      floppy and hard disk image formats;  rom/  the BIOS ROM;  util/  string helpers
# PC98_NEWCPU: the CPU in cpu/ (always 1).
if(NOT DEFINED PC98_NEWCPU)
    set(PC98_NEWCPU 1)
endif()
# PC98_FULLCPU=0 (default) purges TSS task switching / task gates, call-gate privilege transitions,
# the V86 hooks and V30 8080 emulation (reaching one logs and stops); -DPC98_FULLCPU=1 builds the full set.
if(NOT DEFINED PC98_FULLCPU)
    set(PC98_FULLCPU 0)
endif()
set(NEWCPU_SRCS "")
if(PC98_NEWCPU)
    # PC98_FAST16_CMINI=1: the fast16 mini loop in C only (fast16_mini.inc), without the assembly handlers (comparison builds)
    if(PC98_FAST16_CMINI)
        file(GLOB NEWCPU_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/cpu/*.c)
    else()
        file(GLOB NEWCPU_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/cpu/*.c ${CMAKE_CURRENT_SOURCE_DIR}/cpu/*.S)
    endif()
endif()

idf_component_register(
    SRCS rom/rom.c util/pcstr.c machine.c bus.c config.c events.c io.c dev/pic.c dev/pit.c dev/ppi.c dev/rtc.c dev/kbd.c dev/keyunit.c dev/mouse.c dev/dma8237.c media/fdmedia.c media/fdm_d88.c media/fdm_raw.c media/drives.c dev/fdc765.c media/hddmedia.c dev/sasi.c dev/sasi_bind.c dev/ide.c dev/cgwin.c video/crtc.c video/vram.c video/egc.c video/gdc.c video/gdcfig.c video/cgrom.c video/textview.c video/grphview.c video/compose.c sound/mixer.c sound/speaker.c sound/board26k.c sound/ssg.c sound/fm.c sound/orchestra.c sound/orchestra_sampler.c dev/sysctl.c dev/artic.c ${NEWCPU_SRCS}
    INCLUDE_DIRS . include
    REQUIRES esp_rom esp_timer
    LDFRAGMENTS linker.lf
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PC98_FULLCPU=${PC98_FULLCPU}
    PC98_NEWCPU=${PC98_NEWCPU}
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Werror=implicit-function-declaration
    -fno-strict-aliasing)

if(PC98_FAST16_CMINI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PC98_FAST16_CMINI=1)
endif()
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/sound/samples/orchestra_bank.bin" BINARY RENAME_TO pc98_orchestra_bank)
