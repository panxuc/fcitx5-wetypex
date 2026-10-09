#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
version=$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' "$project_root/CMakeLists.txt" | head -n1)
output="$project_root/dist"
stage="$output/root"

mkdir -p "$output"
find "$output" "$output/arch" "$output/debian" "$output/fedora" "$output/ubuntu20" \
    -maxdepth 1 -type f \
    \( -name 'fcitx5-wetypex-*.tar.gz' -o \
       -name 'fcitx5-wetypex-*.tar.zst' -o \
       -name 'fcitx5-wetypex-*.rpm' -o \
       -name 'fcitx5-wetypex_*.deb' -o \
       -name 'fcitx5-wetypex-legacy_*.deb' -o \
       -name 'fcitx5-wetypex-*.pkg.tar.zst' \) \
    -delete 2>/dev/null || true
rm -rf "$stage"
mkdir -p "$stage"
"$project_root/packaging/stage.sh" "$stage"

tar --zstd -C "$stage" -cf "$output/fcitx5-wetypex-${version}-linux-x86_64.tar.zst" .

if command -v nfpm >/dev/null 2>&1 && command -v docker >/dev/null 2>&1; then
    "$project_root/packaging/build-native-packages.sh"
else
    printf '未同时找到 Docker 和 nFPM，跳过原生 deb 与 rpm。\n' >&2
fi

if command -v makepkg >/dev/null 2>&1; then
    python3 "$project_root/tools/package_release.py"
    (cd "$output/arch" && makepkg -C -f --noconfirm)
else
    printf '未找到 makepkg，跳过 Arch 软件包。\n' >&2
fi

(
    cd "$output"
    while IFS= read -r -d '' file; do
        digest=$(sha256sum "$file" | cut -d' ' -f1)
        printf '%s  %s\n' "$digest" "$(basename "$file")"
    done < <(find . ./arch ./debian ./fedora ./ubuntu20 -maxdepth 1 -type f \
        \( -name 'fcitx5-wetypex-*.tar.gz' -o \
           -name 'fcitx5-wetypex-*.tar.zst' -o \
           -name 'fcitx5-wetypex-*.rpm' -o \
           -name 'fcitx5-wetypex_*.deb' -o \
           -name 'fcitx5-wetypex-legacy_*.deb' -o \
           -name 'fcitx5-wetypex-*.pkg.tar.zst' \) \
        -print0 | sort -z)
) >"$output/SHA256SUMS"
