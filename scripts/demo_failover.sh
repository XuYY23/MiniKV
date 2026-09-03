#!/usr/bin/env bash
# 故障演示：编译并运行 test_failover。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ ! -x build/test_failover ]]; then
  cmake -S . -B build
  cmake --build build -j --target test_failover
fi
echo "==== demo_failover: ./build/test_failover ===="
./build/test_failover
