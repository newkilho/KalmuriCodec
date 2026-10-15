#!/bin/bash
# Builds Kalmuri.dll: static LGPL FFmpeg + kff.c (run via build.bat)
set -euo pipefail

FFMPEG_VER=7.1.5
NVHDR_TAG=n12.1.14.0
VPL_TAG=v2.16.0
AMF_TAG=v1.4.36

WORK=$(cygpath -u "${FF_WORK:-D:\\Vendor\\FFMpeg\\build}")
DLL=$(cygpath -u "${FF_DLL:-D:\\Vendor\\Kalmuri}")
SRC=$WORK/src
DEPS=$WORK/deps
OUT=$WORK/static
CODEC=$WORK/codec
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$SRC" "$DEPS/include" "$DEPS/lib" "$DLL" "$CODEC"

PATH="$(dirname "$(which cl)"):$PATH"
export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"

fetch_git() {
    [ -d "$SRC/$1" ] || git -c advice.detachedHead=false clone -q --depth 1 -b "$3" "$2" "$SRC/$1"
}

fetch_git nv-codec-headers https://github.com/FFmpeg/nv-codec-headers "$NVHDR_TAG"
make -s -C "$SRC/nv-codec-headers" install PREFIX="$DEPS"

if [ ! -d "$DEPS/include/AMF" ]; then
    [ -d "$SRC/AMF" ] || git -c advice.detachedHead=false clone -q --depth 1 --filter=blob:none --sparse -b "$AMF_TAG" \
        https://github.com/GPUOpen-LibrariesAndSDKs/AMF "$SRC/AMF"
    git -C "$SRC/AMF" sparse-checkout set amf/public/include
    cp -r "$SRC/AMF/amf/public/include" "$DEPS/include/AMF"
fi

if [ ! -f "$DEPS/lib/vpl-mt.stamp" ]; then
    fetch_git libvpl https://github.com/intel/libvpl "$VPL_TAG"
    cmake -S "$SRC/libvpl" -B "$WORK/vpl-mt" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$DEPS")" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
        -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TOOLS=OFF -DINSTALL_EXAMPLE_CODE=OFF -DBUILD_EXPERIMENTAL=OFF
    cmake --build "$WORK/vpl-mt"
    cmake --install "$WORK/vpl-mt"
    sed -i 's/^Libs.private:.*/Libs.private: -ladvapi32 -lole32/' "$DEPS/lib/pkgconfig/vpl.pc"
    touch "$DEPS/lib/vpl-mt.stamp"
fi

TARBALL=$SRC/ffmpeg-$FFMPEG_VER.tar.xz
[ -f "$TARBALL" ] || curl -sSfLo "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
FF=$WORK/ffmpeg-$FFMPEG_VER
[ -d "$FF" ] || tar -xJf "$TARBALL" -C "$WORK"
cd "$FF"
CONFIG=(
    --toolchain=msvc --arch=x86_64 --target-os=win64
    --prefix="$OUT" --enable-static --disable-shared
    --disable-everything --disable-autodetect
    --disable-programs --disable-doc --disable-network
    --disable-avdevice --disable-avfilter --disable-postproc
    --enable-swscale --enable-swresample
    --enable-encoder=h264_nvenc,h264_qsv,h264_amf,h264_mf,aac
    --enable-muxer=mp4 --enable-protocol=file
    --enable-ffnvcodec --enable-nvenc --enable-libvpl --enable-amf --enable-mediafoundation
    --extra-cflags="-I$(cygpath -m "$DEPS/include") -MT"
    --extra-ldflags="-LIBPATH:$(cygpath -m "$DEPS/lib")"
    --pkg-config-flags=--static
)
if [ ! -f ffbuild/config.mak ] || [ "$(cat .kalmuri-config 2>/dev/null)" != "${CONFIG[*]}" ]; then
    ./configure "${CONFIG[@]}"
    echo "${CONFIG[*]}" > .kalmuri-config
fi
make -j"$(nproc)"
make install
grep -E "^(CONFIG_H264_(NVENC|QSV|AMF|MF)_ENCODER|CONFIG_AAC_ENCODER|CONFIG_MP4_MUXER|CONFIG_GPL|CONFIG_NONFREE)=" ffbuild/config.mak || true

LINKLIBS=()
for t in $(PKG_CONFIG_PATH="$OUT/lib/pkgconfig:$PKG_CONFIG_PATH" pkg-config --static --libs libavformat libavcodec libswscale libswresample libavutil); do
    case "$t" in
        -libpath:*) LINKLIBS+=("-LIBPATH:$(cygpath -m "${t#-libpath:}")") ;;
        -L*) LINKLIBS+=("-LIBPATH:$(cygpath -m "${t#-L}")") ;;
        -l*) n=${t#-l}; if [ -f "$OUT/lib/lib$n.a" ]; then LINKLIBS+=("$(cygpath -m "$OUT/lib/lib$n.a")"); else LINKLIBS+=("$n.lib"); fi ;;
        *) LINKLIBS+=("$t") ;;
    esac
done
cd "$CODEC"
cl -nologo -O2 -MT -W3 -utf-8 -c "$(cygpath -m "$HERE/kff.c")" -I"$(cygpath -m "$OUT/include")" -Fokff.obj
# File version = FFmpeg version + .0 (the DLL holds only FFmpeg for now)
VER="${FFMPEG_VER//./,},0"
cat > kalmuri.rc <<EOF
1 VERSIONINFO
FILEVERSION $VER
PRODUCTVERSION $VER
FILEOS 0x40004
FILETYPE 0x2
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "Kilho.net"
      VALUE "FileDescription", "Kalmuri codec (FFmpeg $FFMPEG_VER, LGPL-2.1-or-later)"
      VALUE "FileVersion", "$FFMPEG_VER.0"
      VALUE "OriginalFilename", "Kalmuri.dll"
      VALUE "ProductName", "Kalmuri"
      VALUE "ProductVersion", "$FFMPEG_VER.0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
EOF
rc -nologo -fokalmuri.res kalmuri.rc
link -nologo -dll -out:Kalmuri.dll -implib:Kalmuri.lib kff.obj kalmuri.res "${LINKLIBS[@]}" -opt:ref -opt:icf
echo "${CONFIG[*]}" > "$CODEC/CONFIGURE.txt"
mv -f Kalmuri.dll "$DLL/"
ls -la "$DLL/Kalmuri.dll" Kalmuri.lib
