#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$script_dir/build/stand"
cmake -S "$script_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target pyro_stand -j"$(nproc)"
echo "Готово: $build_dir/pyro_stand"
echo "Запуск: $build_dir/pyro_stand --debug-socket kasupp-debug-events-v1"
