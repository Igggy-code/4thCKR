#!/usr/bin/env bash
# Fetches the third-party sources that are not stored in this repository:
#   SDL2 (2.32.x), glslang (pinned commit), FFmpeg 7.1 (trimmed static build
#   for the XMV movie player).
#
#   NDK=/path/to/android-ndk ./scripts/fetch_deps.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SDL_COMMIT=2e5b9a860e69afc9fa5259c7b1f0bd52857d475e       # release-2.32.x (2.32.11)
GLSLANG_COMMIT=e1b562a8bed273a02f30b59b66a5d499793cede5

fetch_commit() {  # url dir commit
    if [ -d "$2/.git" ]; then return; fi
    git init -q "$2"
    git -C "$2" remote add origin "$1"
    git -C "$2" fetch -q --depth 1 origin "$3"
    git -C "$2" checkout -q FETCH_HEAD
}

echo "== SDL2"
fetch_commit https://github.com/libsdl-org/SDL.git SDL2 "$SDL_COMMIT"

echo "== glslang"
fetch_commit https://github.com/KhronosGroup/glslang.git nv2a-port/third_party/glslang "$GLSLANG_COMMIT"

echo "== FFmpeg 7.1"
FF=conker/third_party/ffmpeg
if [ ! -f "$FF/lib/arm64-v8a/libavcodec.a" ]; then
    [ -f "$FF/ffmpeg-7.1.tar.xz" ] || curl -L -o "$FF/ffmpeg-7.1.tar.xz" https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
    : "${NDK:?set NDK to the Android NDK (28.0.13004108) to build FFmpeg}"
    NDK="$NDK" sh "$FF/build_ffmpeg_android.sh"
fi
echo "dependencies ready"
