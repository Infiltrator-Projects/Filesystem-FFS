#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
module=$(realpath "${1:?installed FFS.so}")
oracle=$(realpath "${2:?pinned amitools directory}")
files=$(realpath "${3:?kernel and initramfs directory}")
probe=$(realpath "${4:?V2 media exerciser}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
# Native 512-byte FFS has 72 pointers per file header or list block.
PYTHONPATH="$oracle" python3 "$oracle/bin/xdftool" clean.hdf create size=64Mi + format Test ffs \
    + makedir boot + write "$files/vmlinuz" boot/vmlinuz + write "$files/initramfs" boot/initramfs
objcopy --dump-section .infiltrator_boot=profile.elf "$module" copy.so
test -s profile.elf
python3 - <<'MEDIA'
import mmap,struct,subprocess
with open('clean.hdf','rb') as f, mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as disk:
    header=next(pos for pos in range(0,len(disk),512) if disk[pos+432:pos+440]==b'\x07vmlinuz'
                and struct.unpack_from('>I',disk,pos)[0]==2)
    extension=struct.unpack_from('>I',disk,header+504)[0]
    assert extension!=0
    page=list(struct.unpack_from('>128I',disk,extension*512))
    assert page[0]==16 and page[4]==0 and page[125]==header//512 and sum(page)%2**32==0
for mode in ('checksum','owner','cycle'):
    filename=mode+'.img'
    subprocess.run(['cp','--reflink=auto','--sparse=always','clean.hdf',filename],check=True)
    words=page.copy()
    if mode=='owner': words[125]=words[1]
    if mode=='cycle': words[126]=extension
    if mode=='checksum': words[5]^=1
    else:
        words[5]=0
        words[5]=-sum(words) % 2**32
    with open(filename,'r+b') as f:
        f.seek(extension*512)
        f.write(struct.pack('>128I',*words))
MEDIA
sha256sum clean.hdf *.img > hashes
for profile in "$module" "$work/profile.elf"; do
    for sector in 512 4096; do
        "$probe" "$profile" clean.hdf "$sector" clean "$files/vmlinuz" "$files/initramfs"
        for image in checksum owner cycle; do
            "$probe" "$profile" "$image.img" "$sector" bad-data "$files/vmlinuz" "$files/initramfs"
        done
    done
done
sha256sum -c hashes
