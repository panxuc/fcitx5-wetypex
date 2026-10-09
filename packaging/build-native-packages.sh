#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
version=$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' "$project_root/CMakeLists.txt" | head -n1)
revision=$(sed -n 's/^pkgrel=//p' "$project_root/packaging/PKGBUILD.in")
output="$project_root/dist"
docker_network_args=()
if [[ -n "${WETYPEX_DOCKER_NETWORK:-}" ]]; then
    docker_network_args=(--network "$WETYPEX_DOCKER_NETWORK")
fi

command -v docker >/dev/null 2>&1 || {
    printf '需要 Docker 才能在目标发行版环境构建 deb 和 rpm。\n' >&2
    exit 1
}
command -v nfpm >/dev/null 2>&1 || {
    printf '需要 nFPM 才能生成 deb 和 rpm。\n' >&2
    exit 1
}

mkdir -p "$output/debian" "$output/fedora"
rm -rf "$output/debian-root" "$output/fedora-root"

docker build "${docker_network_args[@]}" -q -f "$project_root/packaging/docker/debian.Dockerfile" \
    -t wetypex-build-debian "$project_root" >/dev/null
docker run --rm "${docker_network_args[@]}" --user "$(id -u):$(id -g)" \
    -e WETYPEX_BUILD_JOBS="${WETYPEX_BUILD_JOBS:-2}" \
    -v "$project_root:/src:ro" -v "$output:/out" \
    wetypex-build-debian \
    /src/packaging/container-stage.sh /out/debian-root lib

docker build "${docker_network_args[@]}" -q -f "$project_root/packaging/docker/fedora.Dockerfile" \
    -t wetypex-build-fedora "$project_root" >/dev/null
docker run --rm "${docker_network_args[@]}" --user "$(id -u):$(id -g)" \
    -e WETYPEX_BUILD_JOBS="${WETYPEX_BUILD_JOBS:-2}" \
    -v "$project_root:/src:ro" -v "$output:/out" \
    wetypex-build-fedora \
    /src/packaging/container-stage.sh /out/fedora-root lib64

sed -e "s|@WETYPEX_VERSION@|$version|g" \
    -e "s|@WETYPEX_STAGE@|$output/debian-root|g" \
    -e "s|@WETYPEX_PROJECT_ROOT@|$project_root|g" \
    "$project_root/packaging/nfpm.yaml" >"$output/nfpm-debian.yaml"
nfpm package --config "$output/nfpm-debian.yaml" --packager deb \
    --target "$output/debian/"

sed -e "s|@WETYPEX_VERSION@|$version|g" \
    -e "s|@WETYPEX_STAGE@|$output/fedora-root|g" \
    -e "s|@WETYPEX_PROJECT_ROOT@|$project_root|g" \
    "$project_root/packaging/nfpm.yaml" >"$output/nfpm-fedora.yaml"
nfpm package --config "$output/nfpm-fedora.yaml" --packager rpm \
    --target "$output/fedora/"

mkdir -p "$output/ubuntu20"
rm -rf "$output/ubuntu20-root"
docker build "${docker_network_args[@]}" -q -f "$project_root/packaging/docker/ubuntu20.Dockerfile" \
    -t wetypex-build-ubuntu20 "$project_root" >/dev/null
docker run --rm "${docker_network_args[@]}" --user "$(id -u):$(id -g)" \
    -e WETYPEX_BUILD_JOBS="${WETYPEX_BUILD_JOBS:-2}" \
    -v "$project_root:/src:ro" -v "$output:/out" \
    wetypex-build-ubuntu20 /src/packaging/container-stage.sh /out/ubuntu20-root lib
sed -e "s|@WETYPEX_VERSION@|$version|g" \
    -e "s|@WETYPEX_STAGE@|$output/ubuntu20-root|g" \
    -e "s|@WETYPEX_PROJECT_ROOT@|$project_root|g" \
    -e 's/libc6 (>= 2.35)/libc6 (>= 2.31)/' \
    -e 's/libstdc++6 (>= 11)/libstdc++6 (>= 9)/' \
    -e 's/libssl3 | libssl3t64/libssl1.1/' \
    "$project_root/packaging/nfpm.yaml" >"$output/nfpm-ubuntu20.yaml"
nfpm package --config "$output/nfpm-ubuntu20.yaml" --packager deb \
    --target "$output/ubuntu20/fcitx5-wetypex-legacy_${version}-${revision}_amd64.deb"
