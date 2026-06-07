#!/usr/bin/env bash
# configure, build and test the project in release. pass an alternate build type
# as the first argument (e.g. ./scripts/build.sh Debug).
set -euo pipefail

build_type="${1:-Release}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build"

cmake -S "${root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${build_type}"
cmake --build "${build_dir}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ctest --test-dir "${build_dir}" --output-on-failure
echo
echo "running demo:"
"${build_dir}/hft_demo"
