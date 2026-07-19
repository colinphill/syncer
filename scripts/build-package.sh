#!/usr/bin/env bash
set -euo pipefail

configuration="${1:-Release}"
if [[ "$configuration" != "Release" ]]; then
    echo "Only Release packaging is supported by the macOS and Linux presets." >&2
    exit 2
fi
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "Set ANDROID_NDK_HOME to the Android NDK directory." >&2
    exit 2
fi

case "$(uname -s)" in
    Linux) host_preset=linux ;;
    Darwin) host_preset=macos ;;
    *) echo "This packaging script supports macOS and Linux." >&2; exit 2 ;;
esac

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"

cmake --preset "$host_preset"
cmake --build --preset "$host_preset"

boost_source="$project_root/out/build/$host_preset/_deps/boost-src"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    if [[ -d "$boost_source" ]]; then
        cmake --preset "$abi" "-DFETCHCONTENT_SOURCE_DIR_BOOST=$boost_source"
    else
        cmake --preset "$abi"
    fi
    cmake --build --preset "$abi"
done

package_root="$project_root/out/package/syncer-$configuration-$host_preset"
rm -rf "$package_root"
mkdir -p "$package_root"
cp "$project_root/out/build/$host_preset/syncer" "$package_root/syncer"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    mkdir -p "$package_root/servers/$abi"
    cp "$project_root/out/build/android-$abi/syncerd" "$package_root/servers/$abi/syncerd"
done
echo "Package created at $package_root"
