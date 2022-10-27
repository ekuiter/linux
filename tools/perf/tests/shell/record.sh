#!/bin/sh
# perf record tests
# SPDX-License-Identifier: GPL-2.0

set -e

err=0
perfdata=$(mktemp /tmp/__perf_test.perf.data.XXXXX)

cleanup() {
  rm -f "${perfdata}"
  rm -f "${perfdata}".old
  trap - EXIT TERM INT
}

trap_cleanup() {
  cleanup
  exit 1
}
trap trap_cleanup EXIT TERM INT

test_per_thread() {
  echo "Basic --per-thread mode test"
  if ! perf record -o /dev/null --quiet true 2> /dev/null
  then
    echo "Per-thread record [Skipped event not supported]"
    if [ $err -ne 1 ]
    then
      err=2
    fi
    return
  fi
  if ! perf record --per-thread -o "${perfdata}" true 2> /dev/null
  then
    echo "Per-thread record [Failed record]"
    err=1
    return
  fi
  if ! perf report -i "${perfdata}" -q | grep -q true
  then
    echo "Per-thread record [Failed missing output]"
    err=1
    return
  fi
  echo "Basic --per-thread mode test [Success]"
}

test_register_capture() {
  echo "Register capture test"
  if ! perf list | grep -q 'br_inst_retired.near_call'
  then
    echo "Register capture test [Skipped missing event]"
    if [ $err -ne 1 ]
    then
      err=2
    fi
    return
  fi
  if ! perf record --intr-regs=\? 2>&1 | grep -q 'available registers: AX BX CX DX SI DI BP SP IP FLAGS CS SS R8 R9 R10 R11 R12 R13 R14 R15'
  then
    echo "Register capture test [Skipped missing registers]"
    return
  fi
  if ! perf record -o - --intr-regs=di,r8,dx,cx -e br_inst_retired.near_call:p \
    -c 1000 --per-thread true 2> /dev/null \
    | perf script -F ip,sym,iregs -i - 2> /dev/null \
    | grep -q "DI:"
  then
    echo "Register capture test [Failed missing output]"
    err=1
    return
  fi
  echo "Register capture test [Success]"
}

test_per_thread
test_register_capture

cleanup
exit $err
