#!/usr/bin/env bash
# Builds the APK (after fetch_deps.sh and generate.sh).
#
#   ANDROID_HOME=/path/to/sdk ./scripts/build.sh
#
# Needs NDK 28.0.13004108 and CMake 3.31.6 (SDK manager), JDK 17+.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ls "$ROOT"/conker/src/recomp/gen/recomp_0000.c >/dev/null 2>&1 || {
    echo "no generated code: run scripts/generate.sh /path/to/default.xbe first"; exit 1; }
cd "$ROOT/conker-android"
./build_native.sh
cp app/build/outputs/apk/release/app-release.apk "$ROOT/4thCKR.apk"
echo "APK: $ROOT/4thCKR.apk  (contains recompiled game code: for your own use only)"
