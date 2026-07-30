#!/usr/bin/env bash
# Configure the thingino 3.10.14 kernel to match the Archon vermagic, then
# modules_prepare so we can build wireguard-linux-compat against it.
set -e
TC="$HOME/x-tools/mipsel-linux-musl-cross/bin"
export PATH="$TC:$PATH"
export ARCH=mips
export CROSS_COMPILE=mipsel-linux-musl-
export LOCALVERSION=-Archon
K="$HOME/kmod/thingino-linux"; cd "$K"

echo "=== which defconfig targets T23? ==="
grep -rl -E 'SOC_T23|CONFIG_.*T23' arch/mips/configs/ 2>/dev/null | head
DEF="${DEF:-isvp_swan_defconfig}"
echo "using defconfig: $DEF"
make "$DEF" >/dev/null 2>&1 || { echo "defconfig failed"; exit 1; }

# match Archon vermagic: preempt, NO smp, mod_unload, NO modversions, MIPS32_R1
scripts/config --enable  MODULES
scripts/config --enable  MODULE_UNLOAD
scripts/config --disable MODVERSIONS
scripts/config --enable  PREEMPT
scripts/config --disable SMP
scripts/config --enable  CPU_MIPS32_R1
scripts/config --disable CPU_MIPS32_R2
scripts/config --set-str LOCALVERSION "-Archon"
yes "" | make oldconfig >/dev/null 2>&1 || make olddefconfig >/dev/null 2>&1 || true

echo "=== resulting version bits ==="
grep -E 'CONFIG_LOCALVERSION=|CONFIG_PREEMPT=|CONFIG_SMP|CONFIG_MODVERSIONS|CONFIG_MODULE_UNLOAD=|CONFIG_CPU_MIPS32_R1=' .config

echo "=== modules_prepare (build host tools + headers) ==="
make -j"$(nproc)" modules_prepare 2>&1 | tail -25
echo "--- did it produce the tools? ---"
ls -l scripts/mod/modpost 2>/dev/null && echo "MODPOST_OK" || echo "MODPOST_MISSING"
echo "PREP_DONE"
