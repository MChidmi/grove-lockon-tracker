#!/usr/bin/env bash
# Build the Grove SD demo firmware and package it as a flashable image.
#
#   ./build.sh 1           demo-1: SD card file manager     -> out/grove-sd-demo1.img
#   ./build.sh 2           demo-2: demo-1 + camera capture   -> out/grove-sd-demo2.img
#   ./build.sh 2 --flash   build, then flash it (stop the web server or press "Release port" first)
#
# Needs the Himax SDK, the ARM GNU toolchain and box64 under $GROVE_FW_DIR
# (default ~/grove-ai-fw). The SDK's image tool is x86-64 only, so it runs
# through box64 on this ARM64 Jetson.
set -euo pipefail

DEMO="${1:-1}"
[[ "$DEMO" == 1 || "$DEMO" == 2 ]] || { echo "usage: $0 1|2" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
FW="${GROVE_FW_DIR:-$HOME/grove-ai-fw}"
SDK="$FW/sdk"
APP_DIR="$SDK/EPII_CM55M_APP_S/app/scenario_app/grove_sd_demo"
MAIN_C="$SDK/EPII_CM55M_APP_S/app/main.c"
ELF="$SDK/EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf"
IMG_TOOL="$SDK/we2_image_gen_local"
BOX64="$FW/toolchain/box64/usr/bin/box64"
export PATH="$FW/toolchain/arm-gnu-toolchain-13.2.Rel1-aarch64-arm-none-eabi/bin:$PATH"

# 1. Put the app into the SDK tree and give main.c an entry point for it.
mkdir -p "$APP_DIR"
rsync -a --delete "$HERE/grove_sd_demo/" "$APP_DIR/"
# Our SD card driver, as a FatFs port the app selects with FATFS_PORT_LIST = grove_sd.
rsync -a --delete "$HERE/fatfs_port/grove_sd/" "$SDK/EPII_CM55M_APP_S/middleware/fatfs/port/grove_sd/"
if ! grep -q GROVE_SD_DEMO "$MAIN_C"; then
  cat >> "$MAIN_C" <<'EOF'

#ifdef GROVE_SD_DEMO
#include "grove_sd_demo.h"
/** main entry */
int main(void)
{
	board_init();
	app_main();
	return 0;
}
#endif
EOF
fi

# 2. Compile. Clean first: the two demos build the same files with different defines.
cd "$SDK/EPII_CM55M_APP_S"
make clean >/dev/null
make -j"$(nproc)" APP_TYPE=grove_sd_demo GROVE_DEMO="$DEMO" 2>&1 | tee "$HERE/out-build.log" | grep -E "error|warning: implicit|undefined reference" || true
[[ -f "$ELF" ]] || { echo "Build failed, see $HERE/out-build.log" >&2; exit 1; }
arm-none-eabi-size "$ELF"

# 3. Wrap the ELF into the signed boot image the board's bootloader accepts.
cd "$IMG_TOOL"
chmod +x we2_local_image_gen 2>/dev/null || true
# The tool runs its bundled x86 objcopy/objdump, which box64 0.2.6 mis-runs (its getopt
# emulation leaves optind at 0, so every argument is read as a file name). On every run the
# tool copies them from arm_none_eabi/ into this folder, writing through whatever is there,
# so swap the *sources* for plain copies of native ARM64 binutils (originals kept as *.x86).
# Never use symlinks here: the tool would overwrite their targets.
for t in objcopy objdump; do
  src="arm_none_eabi/arm-none-eabi-$t"
  if file -b "$src" | grep -q x86-64; then
    mv "$src" "$src.x86"
    cp "$(command -v arm-none-eabi-$t)" "$src"
  fi
  rm -f "arm-none-eabi-$t"      # the tool recreates it from $src
done
cp "$ELF" input_case1_secboot/
rm -rf output_case1_sec_wlcsp
# BOX64_PREFER_EMULATED: use the x86 libraries bundled in the tool (libexpat in particular).
# With box64 0.2.6's native wrappers, writing layout.xml crashes (SIGILL in an expat callback).
BOX64_PREFER_EMULATED=1 BOX64_NOBANNER=1 "$BOX64" ./we2_local_image_gen project_case1_blp_wlcsp.json > "$HERE/out-imagegen.log" 2>&1 \
  || { echo "Image generation failed, see $HERE/out-imagegen.log" >&2; tail -20 "$HERE/out-imagegen.log" >&2; exit 1; }

mkdir -p "$HERE/out"
cp output_case1_sec_wlcsp/output.img "$HERE/out/grove-sd-demo$DEMO.img"
ls -l "$HERE/out/grove-sd-demo$DEMO.img"

if [[ "${2:-}" == "--flash" ]]; then
  python3 "$HERE/flash.py" "$HERE/out/grove-sd-demo$DEMO.img"
fi
