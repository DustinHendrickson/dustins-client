#!/bin/sh
# Builds the Android APK, bin/Android/WoW.apk: libwow.so with the NDK (cmake/AndroidToolchain.cmake), WowActivity
# with javac and d8, then the APK with aapt2, zipalign and apksigner. No Gradle.
#
#   Tools/Android/build.sh            build
#   Tools/Android/build.sh install    build, install on the connected device or emulator (adb) and start it
#
# Needs the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with platform android-35, build-tools 35 and NDK
# r27, a JDK 17 (JAVA_HOME), cmake and ninja. The APK is signed with a debug key made on first use
# (build/android/debug.keystore).

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
BUILD_TOOLS=$SDK/build-tools/35.0.0
PLATFORM=$SDK/platforms/android-35/android.jar
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/27.2.12479018}
BUILD=$ROOT/build/android
OUT=$ROOT/bin/Android
STAGE=$BUILD/apk

if [ -z "${JAVA_HOME:-}" ] && [ -d /opt/homebrew/opt/openjdk@17 ]; then
    JAVA_HOME=/opt/homebrew/opt/openjdk@17
fi

export JAVA_HOME
export PATH="$JAVA_HOME/bin:$PATH"

# The game.
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/AndroidToolchain.cmake"
ninja -C "$BUILD"

rm -rf "$STAGE"
mkdir -p "$STAGE/classes" "$STAGE/res" "$STAGE/compiled" "$STAGE/lib/arm64-v8a"

# WowActivity.
javac -source 17 -target 17 -Xlint:-options -classpath "$PLATFORM" -d "$STAGE/classes" \
    $(find "$ROOT/Tools/Android/java" -name '*.java')
"$BUILD_TOOLS/d8" --release --min-api 30 --lib "$PLATFORM" --output "$STAGE" $(find "$STAGE/classes" -name '*.class')

# The launcher icon, from the Windows client's.
for density in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    name=${density%%:*}
    size=${density##*:}
    mkdir -p "$STAGE/res/mipmap-$name"
    sips -s format png -Z "$size" "$ROOT/waystone.ico" --out "$STAGE/res/mipmap-$name/ic_launcher.png" > /dev/null
done

"$BUILD_TOOLS/aapt2" compile --dir "$STAGE/res" -o "$STAGE/compiled/res.zip"
"$BUILD_TOOLS/aapt2" link -o "$STAGE/unaligned.apk" -I "$PLATFORM" --manifest "$ROOT/Tools/Android/AndroidManifest.xml" \
    "$STAGE/compiled/res.zip"

# libwow.so without its symbols (bin/Android keeps them, for crash addresses), stored uncompressed so it loads straight
# from the APK (extractNativeLibs="false").
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libwow.so" "$OUT/libwow.so"

(cd "$STAGE" && zip -q unaligned.apk classes.dex && zip -q -0 unaligned.apk lib/arm64-v8a/libwow.so)

# 16 KB pages for the library, as Android 15 devices may use.
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$STAGE/unaligned.apk" "$STAGE/aligned.apk"

KEYSTORE=$BUILD/debug.keystore

if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias debug -keyalg RSA \
        -keysize 2048 -validity 10000 -dname "CN=wowdecomp debug" > /dev/null
fi

mkdir -p "$OUT"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android --out "$OUT/WoW.apk" \
    "$STAGE/aligned.apk"
rm -f "$OUT/WoW.apk.idsig"

echo "Built $OUT/WoW.apk"

if [ "${1:-}" = "install" ]; then
    ADB=$SDK/platform-tools/adb
    "$ADB" install -r "$OUT/WoW.apk"
    "$ADB" shell am start -n org.wowdecomp.wow/.WowActivity
fi
