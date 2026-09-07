#!/usr/bin/env bash
# Run the memory_sanitizer suite with the karac-emitted object INSTRUMENTED by
# AddressSanitizer, and gate on the result against its own expected-failures
# list. B-2026-09-07-40.
#
# WHY A THIRD LEG. `tests/memory_sanitizer.rs` links `-fsanitize=address` but
# the flag is passed at the LINK step only, and ASAN's memory-ACCESS checking is
# a COMPILER pass. So the default suite is an ALLOCATOR gate — LeakSanitizer,
# double free, invalid free, allocator-side overflow — and is structurally blind
# to an invalid read or write. Measured on B-2026-09-07-33: a callee that wrote
# three zero words into a 56-byte block it had just freed passed this suite at
# both opt levels, while valgrind reported `Invalid write of size 8` at offsets
# 0/40/48. `KARAC_SANITIZE_ADDRESS=1` runs LLVM's `asan` module pass over the
# emitted module (`codegen::driver::apply_address_sanitizer`), which is what
# adds the shadow-memory checks that catch that class.
#
# WHY IT RUNS AT -O0. The instrumentation checks the accesses that SURVIVE
# optimization, and at -O2 a store into freed memory whose result is never read
# is dead and gets deleted — exactly the case above, which passes at -O2 for a
# reason that has nothing to do with correctness. -O0 keeps the allocation and
# the store real, the same argument `asan-o0-leg.sh` makes for the allocator
# gate. Override with ASAN_LEG_OPT_LEVEL if a level-specific question comes up.
#
# WHY A SEPARATE QUARANTINE LIST. This leg sees a strictly larger error class
# than the -O0 leg, so its failure set is a superset and cannot share that
# list — merging them would let an access-checking regression hide behind an
# allocator-class quarantine entry. Same ratchet in both directions: an unlisted
# failure is red, and a listed fixture that starts PASSING is red too. Every
# entry names the bug row that owns it; the list only shrinks.
#
# The mechanism has its own positive control INSIDE the suite:
# `asan_instrumentation_tracks_the_sanitize_address_knob` asserts the emitted
# object carries an `__asan_report*` reference exactly when the knob is set. It
# is not skippable, so a knob that silently stopped instrumenting fails this leg
# rather than turning it green over an uninstrumented run.
#
# Usage:
#   scripts/asan-instrumented-leg.sh                  # full leg
#   scripts/asan-instrumented-leg.sh --update         # rewrite the list from this run
#   ASAN_O0_TEST_THREADS=8 scripts/asan-instrumented-leg.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export KARAC_SANITIZE_ADDRESS=1
export ASAN_LEG_NAME="instrumented -O0"
export ASAN_LEG_OPT_LEVEL="${ASAN_LEG_OPT_LEVEL:-0}"
export ASAN_LEG_EXPECTED="${ASAN_LEG_EXPECTED:-$HERE/../tests/asan-instrumented-known-failures.txt}"

exec "$HERE/asan-o0-leg.sh" "$@"
