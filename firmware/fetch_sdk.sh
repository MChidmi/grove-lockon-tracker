#!/usr/bin/env bash
# Fetch the parts of the Himax SDK needed to build the Grove SD demo firmware, in small
# batches with retries (the network here drops long transfers).
# Usage: ./fetch_sdk.sh   (into $GROVE_FW_DIR/sdk, default ~/grove-ai-fw/sdk; build.sh looks there)
set -u
FW="${GROVE_FW_DIR:-$HOME/grove-ai-fw}"
mkdir -p "$FW/sdk"
cd "$FW/sdk" || exit 1
if [ ! -d .git ]; then
  git init -q
  git remote add origin https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2.git
  git config core.sparseCheckout true
  git fetch -q --depth 1 --filter=blob:none origin main || exit 1
  git checkout -q -b main FETCH_HEAD 2>/dev/null || git branch -q main FETCH_HEAD
fi
git config http.version HTTP/1.1

E=EPII_CM55M_APP_S
patterns=(
  "/README.md" "/FAQ.md"
  "/$E/makefile" "/$E/options/" "/$E/app/*.c" "/$E/app/*.h" "/$E/app/*.mk" "/$E/app/scenario_app/*.mk"
  "/$E/CMSIS/" "/$E/board/" "/$E/cmsis_drivers/" "/$E/customer/" "/$E/device/" "/$E/interface/"
  "/$E/linker_script/" "/$E/trustzone/" "/$E/external/" "/$E/middleware/" "/$E/drivers/"
  "/$E/library/common/" "/$E/library/hxevent/" "/$E/library/pwrmgmt/" "/$E/library/sensordp/"
  "/$E/library/spi_ptl/" "/$E/library/spi_eeprom/" "/$E/library/tpgdp/" "/$E/library/JPEGENC/"
  "/$E/library/img_proc/" "/$E/library/i2c_comm/" "/$E/library/security/" "/$E/library/ccontrol/"
  "/$E/library/hxmb/" "/$E/library/spi_psram/" "/$E/library/audio/" "/$E/library/cmsis_cv/"
  "/$E/prebuilt_libs/gnu/lib[!t]*" "/$E/prebuilt_libs/gnu/libtpgdp.a" "/$E/prebuilt_libs/gnu/libtrustzone_cfg.a"
  "/$E/os/"
  "/$E/app/scenario_app/fatfs_mmc_spi/" "/$E/app/scenario_app/allon_jpeg_encode/"
  "/$E/app/scenario_app/allon_sensor_tflm_fatfs/" "/$E/app/scenario_app/event_handler/"
  "/xmodem/"
)

git sparse-checkout set --no-cone "${patterns[0]}"
for p in "${patterns[@]}"; do
  git sparse-checkout add "$p" 2>/dev/null
  for try in $(seq 1 8); do
    if git checkout -q 2>/dev/null; then echo "ok   $p"; break; fi
    echo "retry $try $p"; sleep 3
  done
done
echo "SDK sources: $(find . -path ./.git -prune -o -type f -print | wc -l) files, $(du -sh . | cut -f1)"

# Image tool binaries: large single files, fetched with resumable curl.
mkdir -p we2_image_gen_local
base="https://raw.githubusercontent.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/main/we2_image_gen_local"
for f in $(git ls-tree --name-only HEAD we2_image_gen_local/ | sed 's|we2_image_gen_local/||' | grep -vE '\.exe$|macOS|^output'); do
  [ "$(git ls-tree HEAD "we2_image_gen_local/$f" | awk '{print $2}')" = tree ] && continue
  for try in $(seq 1 15); do
    curl -sS -L --retry 5 --retry-all-errors -C - -o "we2_image_gen_local/$f" "$base/$f" && { echo "ok   we2_image_gen_local/$f"; break; }
    echo "retry $try we2_image_gen_local/$f"; sleep 3
  done
done
# Subfolders of the image tool (configs, keys)
for d in $(git ls-tree -d --name-only HEAD we2_image_gen_local/); do
  git sparse-checkout add "/$d/"
  for try in $(seq 1 8); do git checkout -q 2>/dev/null && { echo "ok   $d"; break; }; sleep 3; done
done
echo DONE
