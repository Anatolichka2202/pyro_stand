#!/usr/bin/env bash
# build.sh — сборка pyro_stand и/или pyro_demo
# Использование:
#   ./build.sh            — только pyro_stand (production)
#   ./build.sh demo       — только pyro_demo (демо без железа)
#   ./build.sh all        — pyro_stand + pyro_demo

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
MODE="${1:-stand}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

case "$MODE" in
  demo)
    echo "=== Настройка: pyro_demo (демо-режим) ==="
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON
    echo "=== Сборка: pyro_demo ==="
    cmake --build . --target pyro_demo -j"$(nproc)"
    echo ""
    echo "Готово: $BUILD_DIR/pyro_demo"
    echo "Запуск: $BUILD_DIR/pyro_demo"
    ;;
  all)
    echo "=== Настройка: pyro_stand + pyro_demo ==="
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON
    echo "=== Сборка: все цели ==="
    cmake --build . --target pyro_stand pyro_demo -j"$(nproc)"
    echo ""
    echo "Готово:"
    echo "  $BUILD_DIR/pyro_stand"
    echo "  $BUILD_DIR/pyro_demo"
    ;;
  *)
    echo "=== Настройка: pyro_stand (production) ==="
    cmake .. -DCMAKE_BUILD_TYPE=Release
    echo "=== Сборка: pyro_stand ==="
    cmake --build . --target pyro_stand -j"$(nproc)"
    echo ""
    echo "Готово: $BUILD_DIR/pyro_stand"
    echo "Запуск: $BUILD_DIR/pyro_stand --port /dev/ttyUSB0"
    ;;
esac
