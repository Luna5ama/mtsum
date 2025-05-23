#!/bin/bash

# shellcheck disable=SC2164

root_dir=$(pwd)
releases_dir="${root_dir}/releases"
version=$(cat version.txt)

mkdir -p "$releases_dir"

function build_and_archive() {
    local triplet=$1
    cmake --build .
    archive_name="mtsum-v${version}-${triplet}"
    tar -cf - "mtsum" > "${archive_name}.tar"
    7z a -tgzip -mx=9 "${releases_dir}/${archive_name}.tar.gz" "${archive_name}.tar"
    rm "${archive_name}.tar"
}

cmake --preset release-make -DMTSUM_STATIC=OFF -DMTSUM_VCPKG=ON -B cmake-build-release-x64-linux
cd cmake-build-release-x64-linux
build_and_archive "x64-linux"

cd "$root_dir"

cmake --preset release-make -DMTSUM_STATIC=ON -DMTSUM_VCPKG=ON -B cmake-build-release-x64-linux-static
cd cmake-build-release-x64-linux-static
build_and_archive "x64-linux-static"
