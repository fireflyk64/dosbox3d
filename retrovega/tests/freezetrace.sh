#!/bin/bash
# What a frozen run was doing (tests/monkey.sh): its stack by gdb, and for a DOS game the CPU's CS:IP as DOSBox has it
#   tests/freezetrace.sh PID [RVLOG.TXT]
pid=$1; log=${2:-}
echo "--- the last of the log"; [ -n "$log" ] && tail -5 "$log" 2>/dev/null | cut -c1-120
echo "--- gdb"
gdb -q -batch -p "$pid" -ex "bt 25" -ex "info threads" \
    -ex 'printf "dos cs:ip %04x:%04x ss:sp %04x:%04x\n", Segs.val[1], cpu_regs.ip.word[0], Segs.val[2], cpu_regs.regs[4].word[0]' 2>&1 \
    | grep -v "^warning\|^\[New\|^\[Thread\|libthread_db" | cut -c1-150
