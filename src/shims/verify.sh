#!/usr/bin/env bash
# ============================================================================
# verify.sh -- assert that a shim .so is loadable by vp_project.
#
#   bash src/shims/verify.sh                 # verify the shipped bin/*.so
#   bash src/shims/verify.sh a.so b.so ...   # verify specific files
#
# build.sh calls this on its own outputs. It is also useful standalone as a
# release gate on bin/ (this is test 1.7 in docs/TESTING.md).
#
# The four assertions are the only properties that actually matter at load
# time inside vp_project. Everything else (SONAME, BIND_NOW, stripped-ness,
# exact byte size) differs between the shipped objects already and is NOT
# checked -- see the flag table in build.sh.
#
#   1. ELF32, little-endian, Type DYN, Machine MIPS, o32 + mips32r2 + pic.
#      vp_project is ET_EXEC non-PIE at a fixed 0x400000 base; the shims are
#      PIC DYN objects the loader maps alongside it.
#   2. ZERO NEEDED entries. A NEEDED here would make ld.so try to pull another
#      library into the process and is an immediate regression.
#   3. Non-empty .init_array. No constructor == the shim silently does nothing.
#   4. Undefined dynamic symbols are a SUBSET of the libc allowlist below.
#      Anything outside it will not resolve against vp_project's uClibc and
#      the process dies at load -- which, for a shim in the boot LD_PRELOAD
#      chain, means the camera does not come up.
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Allowlist = the union of the UND sets of the five shipped bin/*.so, i.e.
# every libc symbol these shims are known to resolve out of vp_project's
# uClibc 0.9.33 / libpthread. Extending a shim to call a new libc function
# means adding it here *and* confirming uClibc actually exports it.
EXPECTED_UND="
__errno_location
atoi
close
getenv
open
pthread_create
pthread_detach
read
rename
sendto
sleep
socket
unsetenv
write
"

READELF="${READELF:-}"
if [ -z "$READELF" ]; then
    for c in "$HOME/x-tools/mipsel-linux-musl-cross/bin/mipsel-linux-musl-readelf" \
             mipsel-linux-musl-readelf readelf; do
        if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then READELF="$c"; break; fi
    done
fi
if [ -z "$READELF" ]; then
    echo "ERROR: no readelf found. Install binutils (host readelf reads MIPS fine)," >&2
    echo "       or set READELF=/path/to/mipsel-linux-musl-readelf." >&2
    exit 1
fi

# Default target set: the shipped prebuilt binaries.
if [ "$#" -gt 0 ]; then
    TARGETS=("$@")
else
    TARGETS=()
    for s in camweb wifi_sd pir_sleep battery_osd mic_capture; do
        TARGETS+=("$HERE/../../bin/$s.so")
    done
fi

fail=0
allowed_re="$(echo "$EXPECTED_UND" | tr -s '[:space:]' '|' | sed 's/^|//; s/|$//')"

for so in "${TARGETS[@]}"; do
    name="$(basename "$so")"
    echo "=== $name"
    if [ ! -f "$so" ]; then
        echo "    FAIL  file does not exist: $so"
        fail=1
        continue
    fi

    hdr="$("$READELF" -h "$so" 2>/dev/null)" || hdr=""
    dyn="$("$READELF" -d "$so" 2>/dev/null)" || dyn=""
    sec="$("$READELF" -SW "$so" 2>/dev/null)" || sec=""
    dsy="$("$READELF" --dyn-syms -W "$so" 2>/dev/null)" || dsy=""

    if [ -z "$hdr" ]; then
        echo "    FAIL  not an ELF file (readelf -h produced nothing)"
        fail=1
        continue
    fi

    # -- 1. ELF shape ------------------------------------------------------
    ok=1
    echo "$hdr" | grep -q 'Class:  *ELF32'                       || { echo "    FAIL  not ELF32"; ok=0; }
    echo "$hdr" | grep -q 'Data:.*little endian'                 || { echo "    FAIL  not little-endian"; ok=0; }
    echo "$hdr" | grep -qE 'Type:  *DYN'                         || { echo "    FAIL  ELF type is not DYN"; ok=0; }
    echo "$hdr" | grep -qi 'Machine:.*MIPS'                      || { echo "    FAIL  machine is not MIPS"; ok=0; }
    flags="$(echo "$hdr" | sed -n 's/^ *Flags: *//p')"
    case "$flags" in *o32*)       ;; *) echo "    FAIL  ABI is not o32 (flags: $flags)";      ok=0 ;; esac
    case "$flags" in *mips32r2*)  ;; *) echo "    FAIL  ISA is not mips32r2 (flags: $flags)"; ok=0 ;; esac
    case "$flags" in *pic*)       ;; *) echo "    FAIL  not PIC (flags: $flags)";             ok=0 ;; esac
    [ "$ok" = 1 ] && echo "    ok    ELF32 LE DYN MIPS [$flags]"
    [ "$ok" = 1 ] || fail=1

    # -- 2. no NEEDED ------------------------------------------------------
    needed="$(echo "$dyn" | grep -c 'NEEDED' || true)"
    if [ "$needed" -ne 0 ]; then
        echo "    FAIL  has $needed NEEDED entr(y|ies) -- must be freestanding:"
        echo "$dyn" | grep 'NEEDED' | sed 's/^/          /'
        fail=1
    else
        echo "    ok    no NEEDED (freestanding)"
    fi

    # -- 3. non-empty .init_array -----------------------------------------
    # Read the section size (hex) rather than INIT_ARRAYSZ so this also holds
    # for a stripped object; both are present, the section is authoritative.
    ia_size="$(echo "$sec" | awk '$2==".init_array" {print $6; exit}')"
    if [ -z "$ia_size" ]; then
        # Fall back to the dynamic tag if section headers were removed.
        ia_size="$(echo "$dyn" | sed -n 's/.*INIT_ARRAYSZ.*[^0-9]\([0-9][0-9]*\) (bytes).*/\1/p')"
    fi
    if [ -z "$ia_size" ]; then
        echo "    FAIL  no .init_array / INIT_ARRAYSZ -- constructor missing"
        fail=1
    elif [ "$(echo "$ia_size" | tr -d '0')" = "" ]; then
        echo "    FAIL  .init_array is empty (size $ia_size) -- constructor missing"
        fail=1
    else
        echo "    ok    .init_array size 0x$ia_size (constructor present)"
    fi

    # -- 4. UND symbols subset of the libc allowlist ----------------------
    # Columns: Num Value Size Type Bind Vis Ndx Name -> Ndx=="UND", Name=$8.
    und="$(echo "$dsy" | awk '$7=="UND" && $8!="" {print $8}' | sed 's/@.*//' | sort -u)"
    bad=""
    for sym in $und; do
        echo "$sym" | grep -qE "^($allowed_re)$" || bad="$bad $sym"
    done
    if [ -n "$bad" ]; then
        echo "    FAIL  undefined symbol(s) outside the uClibc allowlist:$bad"
        fail=1
    else
        echo "    ok    UND ⊆ allowlist: $(echo "$und" | tr '\n' ' ')"
    fi
done

echo
if [ "$fail" -ne 0 ]; then
    echo "VERIFY FAILED"
    exit 1
fi
echo "VERIFY OK (${#TARGETS[@]} object(s))"
