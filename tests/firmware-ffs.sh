#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Boot a real Linux EFI stub via the V2-only FFS reader in disposable OVMF.
set -euo pipefail
export LC_ALL=C
boot=$(realpath "${1:?BOOTX64.EFI required}")
module=$(realpath "${2:?firmware FFS.so required}")
kernel=$(realpath "${3:?Linux EFI stub image required}")
work=$(realpath -m "${4:?output directory required}")
mode=${5:-clean}
case "$mode" in clean|bad-superblock) ;; *) exit 2 ;; esac
ovmf=${OVMF_DIR:-/usr/share/OVMF}
mkdir -p "$work"
esp="$work/esp.img"
media="$work/ffs.img"
disk="$work/gpt.img"
tree="$work/root"
ram="$work/ram"
mkdir -p "$tree/System/Kernel/Images" "$ram"/{bin,dev,proc}
images="$tree/System/Kernel/Images"
cp "$kernel" "$images/native.img"
cp "$(command -v busybox)" "$ram/bin/"
ln -sf busybox "$ram/bin/sh"
cat > "$ram/init" <<'GUEST'
#!/bin/sh
set -eu
b=/bin/busybox
$b mount -t devtmpfs devtmpfs /dev
exec </dev/ttyS0 >/dev/ttyS0 2>&1
trap '/bin/busybox poweroff -f' EXIT
$b mount -t proc proc /proc
$b grep -qw ffs_so_trial /proc/cmdline
echo FFS_COMMON_V2_LINUX_HANDOFF_PASS
GUEST
chmod +x "$ram/init"
(cd "$ram" && find . -print0 | cpio --null -o -H newc --owner=0:0 --quiet | gzip -1) > "$images/native.initrd"
# Test real file contents, not a hardcoded firmware or native reader.
cp "$kernel" "$tree/vmlinuz"
cp "$images/native.initrd" "$images/current.initrd"
# Build a genuine DOS1 FFS volume with Amiga native directory hashes,
# allocation bitmap, checksummed metadata and the exact Linux EFI payloads.
# The test-only generator never participates in BOOT or production FFS.so.
"$(dirname "$work")/ffs-boot-fixture" "$media" "$kernel" "$images/native.initrd"
if [[ $mode == bad-superblock ]]; then
    # Deliberately invalidate DOS1 magic on disposable media.
    printf '\000' | dd of="$media" bs=1 seek=3 conv=notrunc status=none
fi
truncate -s 64M "$esp"
mkfs.fat -F32 "$esp"
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/BOOT/filesystems_x64
mcopy -i "$esp" "$boot" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$module" ::/EFI/BOOT/filesystems_x64/FFS.so
truncate -s 200M "$disk"
sgdisk --clear --new=1:2048:+64M --typecode=1:ef00 \
    --new=2:133120:+128M --typecode=2:8300 "$disk"
partuuid=$(LC_ALL=C sgdisk --info=2 "$disk" | awk '/Partition unique GUID: / { print $4 }')
[[ $partuuid =~ ^[[:xdigit:]]{8}(-[[:xdigit:]]{4}){3}-[[:xdigit:]]{12}$ ]]
cat > "$work/boot.conf" <<BOOT
timeout 1
textonly true
use_nvram false
driver_diagnostics true
scanfor manual
menuentry "InfiltratorOS kernel from FFS.so" {
    volume $partuuid
    filesystem_module ffs
    loader \vmlinuz
    initrd \System\Kernel\Images\current.initrd
    linux_load memory
    ostype Linux
    options "console=ttyS0 rdinit=/init panic=1 ffs_so_trial"
}
BOOT
mcopy -i "$esp" "$work/boot.conf" ::/EFI/BOOT/
dd if="$esp" of="$disk" bs=1M seek=1 conv=notrunc status=none
dd if="$media" of="$disk" bs=1M seek=65 conv=notrunc status=none
cp "$ovmf/OVMF_VARS_4M.fd" "$work/vars.fd"
qemu=(qemu-system-x86_64 -machine q35,accel=tcg -cpu max -m 768 -display none
      -monitor none -net none -no-reboot -serial "file:$work/serial.log"
      -drive "if=pflash,format=raw,readonly=on,file=$ovmf/OVMF_CODE_4M.fd"
      -drive "if=pflash,format=raw,file=$work/vars.fd"
      -drive "file=$disk,format=raw,if=none,id=bootdisk"
      -device nvme,drive=bootdisk,serial=ffs-qualification)
if [[ -n ${QEMU_DATA_DIR:-} ]]; then
    qemu+=(-L "$QEMU_DATA_DIR")
fi
if [[ $mode == bad-superblock ]]; then
    result=0
    timeout 30 "${qemu[@]}" || result=$?
    [[ $result == 0 || $result == 124 ]]
    grep -Fq 'validated FFS.so' "$work/serial.log"
    if grep -Fq 'FFS_COMMON_V2_LINUX_HANDOFF_PASS' "$work/serial.log"; then
        echo 'BOOT read a kernel from a corrupted FFS superblock' >&2
        exit 1
    fi
    dd if="$disk" bs=1M skip=65 count=128 status=none | cmp - "$media"
    echo FFS_COMMON_V2_CORRUPT_SUPERBLOCK_REFUSED
    exit 0
fi
if ! timeout 120 "${qemu[@]}" ||
   ! grep -Fq 'FFS_COMMON_V2_LINUX_HANDOFF_PASS' "$work/serial.log"; then
    tail -n 70 "$work/serial.log" >&2
    exit 1
fi
grep -Fq 'validated FFS.so' "$work/serial.log"
grep -Fq "close /vmlinuz: $(stat -c %s "$kernel") bytes read through module" "$work/serial.log"
grep -Fq "close /System/Kernel/Images/current.initrd: $(stat -c %s "$images/native.initrd") bytes read through module" "$work/serial.log"
dd if="$disk" bs=1M skip=65 count=128 status=none | cmp - "$media"
echo FFS_COMMON_V2_LINUX_HANDOFF_PASS
