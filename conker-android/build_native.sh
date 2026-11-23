#!/usr/bin/env bash
# Build libmain.so (recompiled Conker + xboxrecomp runtime) and libSDL2.so for
# Android arm64, drop them into jniLibs and package the APK.
#
#   ANDROID_HOME=/path/to/sdk ./build_native.sh
#
# Needs: NDK 28.0.13004108 and CMake 3.31.6 from the SDK manager, JDK 17+.
# app/jni/{SDL2,xboxrecomp,conker} are symlinks to the three source trees.
set -euo pipefail
cd "$(dirname "$0")"
: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
NDK="$ANDROID_HOME/ndk/28.0.13004108"
CM="$ANDROID_HOME/cmake/3.31.6/bin"
BUILD="${BUILD_DIR:-build-a64}"
LLVM="$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)"

"$CM/cmake" -S app/jni -B "$BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$CM/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE=Release
"$CM/ninja" -C "$BUILD" -j"$(nproc)" main

OUT=app/src/main/jniLibs/arm64-v8a
mkdir -p "$OUT"
"$LLVM/llvm-strip" --strip-debug    -o "$OUT/libmain.so" "$BUILD/libmain.so"
"$LLVM/llvm-strip" --strip-unneeded -o "$OUT/libSDL2.so" "$BUILD/SDL2/libSDL2.so"

# libadrenotools hook libraries (Turnip on Adreno): loaded by path from the
# app's native library dir into the Vulkan loader's namespace. Static C++
# runtime: they must not need libc++_shared.so, which the APK does not ship.
HOOKS="$BUILD-adrenohooks"
mkdir -p "$HOOKS/src"
cat > "$HOOKS/src/CMakeLists.txt" <<EOT
cmake_minimum_required(VERSION 3.14)
project(adrenohooks C CXX)
add_subdirectory($PWD/app/jni/conker/third_party/adrenotools/lib/linkernsbypass lnb)
add_subdirectory($PWD/app/jni/conker/third_party/adrenotools/src/hook hook)
EOT
"$CM/cmake" -S "$HOOKS/src" -B "$HOOKS" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$CM/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
"$CM/ninja" -C "$HOOKS"
for h in hook_impl main_hook file_redirect_hook gsl_alloc_hook; do
    "$LLVM/llvm-strip" --strip-unneeded -o "$OUT/lib$h.so" "$HOOKS/hook/lib$h.so"
done

./gradlew assembleRelease --no-daemon -q
ls -la app/build/outputs/apk/release/app-release.apk
