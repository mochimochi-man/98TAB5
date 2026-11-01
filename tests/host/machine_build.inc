# SPDX-License-Identifier: MIT
# Copyright (c) mochimochi-man / Uh
# Shared host build of the whole machine (pc98core) used by build_machine_host.sh and build_vmem_run_test.sh.
# Sourced, not executed.  Inputs: ROOT, OBJ (object dir), EXTRA_SOURCES, EXTRA_DEFS, OPT.
machine_compile() {
  local CORE="$ROOT/components/pc98core" HOST="$ROOT/tests/host"
  local INC="-I$HOST/shim_machine -I$CORE -I$CORE/include"
  local DEF="-D_GNU_SOURCE -DPC98_NEWCPU=1 -DPC98_FULLCPU=${PC98_FULLCPU:-0} ${EXTRA_DEFS:-}"
  local FLAGS="-include $HOST/shim_machine/hostdefs.h ${OPT:--O2 -g} -Werror=implicit-function-declaration -fwrapv -fno-strict-aliasing"
  mkdir -p "$OBJ"
  local list="$OBJ/cmds.txt"; : > "$list"
  local f o
  # the firmware's own source list (components/pc98core/CMakeLists.txt, SRCS)
  local SRCLIST; SRCLIST=$(sed -n 's/^ *SRCS \(.*\) \${NEWCPU_SRCS}.*/\1/p' "$CORE/CMakeLists.txt" | tr ' ' '\n' | sed "s#^#components/pc98core/#")
  for f in $SRCLIST $EXTRA_SOURCES; do
    case "$f" in /*) ;; *) f="$ROOT/$f";; esac
    o="$OBJ/$(echo "${f#$ROOT/}" | tr '/' '_').o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "${FORCE:-}" ]; then echo "gcc $FLAGS $DEF $INC -c $f -o $o" >> "$list"; fi
  done
  if [ -s "$list" ]; then xargs -P "${JOBS:-8}" -d '\n' -I{} bash -c '{}' < "$list" || return 1; fi
}
