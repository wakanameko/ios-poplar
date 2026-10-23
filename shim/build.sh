#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock PixelOS header.
set -euo pipefail
cd "$(dirname "$0")"
# ヘッダバージョンが古いからDTBをKernelに連結させる
DTB=${DTB:-../dtb/boot_unpacked/dtb}
CROSS=${CROSS:-aarch64-linux-gnu-}
MKBOOTIMG=${MKBOOTIMG:-mkbootimg}
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Find DTB offs (16)
hdr=$(od -An -t u8 -j 16 -N 8 out/Image | tr -d " ")
[ "$hdr" = "$size" ] || echo "warning: Image header image_size ($hdr) != file size ($size): appended DTB may not be found" >&2
# Ramdisk: compress with gzip to reduce size under 36MiB
if [ -n "${1:-}" ]; then
    echo "Compressing payload ($1) with gzip..."
    gzip -9 -c "$1" > out/ramdisk.bin
    
    # サイズチェック（36MiB = 37748736 バイト以下か確認）
    rd_size=$(stat -c%s "out/ramdisk.bin" 2>/dev/null || stat -f%z "out/ramdisk.bin")
    echo "Compressed ramdisk size: $(( rd_size / 1024 / 1024 )) MiB"
    
    if [ "$rd_size" -gt 37748736 ]; then
        echo "error: Compressed ramdisk still exceeds 36MiB limit!" >&2
        exit 1
    fi
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
# Stockがgzip圧縮だったから
if [ "${KERNEL_GZ:-1}" = 1 ]; then
	gzip -9 -c out/Image | cat - "$DTB" > out/Image-dtb
else
	cat out/Image "$DTB" > out/Image-dtb
fi
# mkbootimg 変更点:
# 	ヘッダバージョンを2から0に変更
# 	
mkbootimg --header_version 0 --kernel out/Image-dtb --ramdisk out/ramdisk.bin \
	--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --pagesize 4096 \
	--os_version 9.0.0 --os_patch_level 2019-02 \
	--cmdline "console=null" -o out/shim-boot.img

# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/Image-dtb out/shim-boot.img out/ramdisk.bin