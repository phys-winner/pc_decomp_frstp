#!/bin/sh
# Build JimVulkan r10 with the CSF tiler OOM handler and Kraid fragment compiler.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/build/gpu-runtime/panvk"
patch_file="$root/android/gpu/panvk-r10.patch"
archive="$out/jim-r10-source.tar.gz"
source_sha=859ad10b834886ba69ba56778bf8425337b0ca72df464e9cddefebf1e77297b9
kraid_archive="$out/mesa-kraid-source.tar.gz"
kraid_revision=e7371b0711bb1cd38c39d1c05e5578c445cc6bca
kraid_sha=cc00dc4318ff456081eaff3f4762e4a049ce90bb08095e0233b56bdab6da72cb
patch_sha=$(sha256sum "$patch_file" | cut -d ' ' -f 1)
work="${XDG_CACHE_HOME:-$HOME/.cache}/frostpunk-panvk/$patch_sha-$kraid_sha"
mkdir -p "$out" "$work"

fetch_archive() (
    destination=$1
    checksum=$2
    if [ ! -f "$destination" ]; then
        curl --fail --location --output "$destination.download" "$3"
        printf '%s  %s\n' "$checksum" "$destination.download" | sha256sum -c -
        mv "$destination.download" "$destination"
    fi
    printf '%s  %s\n' "$checksum" "$destination" | sha256sum -c -
)

fetch_archive "$archive" "$source_sha" 'https://codeload.github.com/JimVulkan/mali-panvk/tar.gz/58f2c55854'
fetch_archive "$kraid_archive" "$kraid_sha" "https://codeload.github.com/chaotic-cx/mesa-mirror/tar.gz/$kraid_revision"
if [ ! -f "$work/.patched" ]; then
    tar -xzf "$archive" --strip-components=1 -C "$work"
    (cd "$work" && patch -p1 < "$patch_file")
    # JimVulkan carries the Kraid interfaces and omits the Rust sources.
    tar -xzf "$kraid_archive" --strip-components=1 --skip-old-files -C "$work" --wildcards \
        '*/src/panfrost/compiler/kraid/*' '*/src/compiler/rust/*' '*/src/util/rust/*' \
        '*/subprojects/*.wrap' '*/subprojects/packagefiles/*'
    touch "$work/.patched"
fi
(cd "$work" && GRALLOC=1 BUILD_DIR=build-android sh ./build.sh "$@")
cp "$work/build-android/libvulkan_panfrost.so" "$out/libvulkan_panfrost-fixed.so"
rust_docs="$(rustc --print sysroot)/share/doc/rust"
{
    cat "$work/android/NOTICE.txt"
    printf '\nKraid from Mesa %s (MIT)\n' "$kraid_revision"
    printf 'Copyright 2026 Collabora, Ltd. and the contributors listed in the Mesa sources.\n'
    for crate in "$work"/subprojects/*; do
        [ -f "$crate/Cargo.toml" ] || continue
        for license in "$crate"/LICENSE*; do
            [ -f "$license" ] || continue
            printf '\n%s / %s\n' "${crate##*/}" "${license##*/}"
            cat "$license"
        done
    done
    for license in "$rust_docs/COPYRIGHT-library.html" "$rust_docs"/licenses/*.txt; do
        [ -f "$license" ] || continue
        printf '\nRust standard library / %s\n' "${license##*/}"
        cat "$license"
    done
} > "$out/NOTICE-fixed.txt"
sha256sum "$patch_file" "$root/tools/build_panvk.sh" "$out/libvulkan_panfrost-fixed.so" "$out/NOTICE-fixed.txt" |
    cut -d ' ' -f 1 > "$out/fixed.sha256"
