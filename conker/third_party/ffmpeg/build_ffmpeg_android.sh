#!/bin/sh
# Rebuilds the trimmed static FFmpeg 7.1 (LGPL) used by the native XMV movie
# player (src/xmv_native.c): XMV demuxer, WMV2 video, WMA/ADPCM/PCM audio.
# usage: NDK=/path/to/ndk ./build_ffmpeg_android.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${NDK:?set NDK}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
W=$(mktemp -d); tar -xJf "$HERE/ffmpeg-7.1.tar.xz" -C "$W"; cd "$W/ffmpeg-7.1"
./configure --prefix="$W/out" --target-os=android --arch=aarch64 --enable-cross-compile \
  --cc=$TC/bin/aarch64-linux-android26-clang --cxx=$TC/bin/aarch64-linux-android26-clang++ \
  --ar=$TC/bin/llvm-ar --ranlib=$TC/bin/llvm-ranlib --strip=$TC/bin/llvm-strip --nm=$TC/bin/llvm-nm \
  --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-avdevice \
  --disable-avfilter --disable-everything --enable-demuxer=xmv \
  --enable-decoder=wmv2,wmav2,wmav1,adpcm_ima_xbox,adpcm_ima_wav,adpcm_ms,pcm_s16le,pcm_u8 \
  --enable-protocol=file --enable-swscale --enable-swresample
make -j"$(nproc)" && make install
cp -r "$W/out/include" "$HERE/"; mkdir -p "$HERE/lib/arm64-v8a"; cp "$W/out/lib/"*.a "$HERE/lib/arm64-v8a/"
