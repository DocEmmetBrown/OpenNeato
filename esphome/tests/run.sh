#!/usr/bin/env bash
# Build and run the OpenNeato ESPHome host unit tests (pure-logic modules).
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-$(command -v clang++ || command -v g++)}"
BUILD_DIR="${BUILD_DIR:-build}"
mkdir -p "$BUILD_DIR"

"$CXX" -std=c++17 -Wall -Wextra -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  ../components/openneato/neato_*.cpp test_*.cpp \
  -o "$BUILD_DIR/openneato_tests"

"$BUILD_DIR/openneato_tests" "$@"
