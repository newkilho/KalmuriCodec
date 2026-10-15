# KalmuriCodec

`Kalmuri.dll` is the video recording codec used by [Kalmuri](https://kalmuri.kilho.net), a screen capture tool for Windows.
It bundles a minimal static build of [FFmpeg](https://ffmpeg.org) 7.1.5 with a thin C wrapper (`kff.c`) into a single DLL.
The file version of the DLL is the FFmpeg version followed by `.0` (for example `7.1.5.0`).

Kalmuri loads this DLL at run time and calls only the `kff_*` functions it exports.
You can rebuild the DLL from this source and replace the copy next to `Kalmuri.exe`.

## License

The DLL as a whole, including `kff.c`, is licensed under the **GNU Lesser General Public License v2.1 or later**. See [LICENSE](LICENSE).

The FFmpeg build is LGPL-only: it is configured without `--enable-gpl` and `--enable-nonfree`, and it contains no x264 or other GPL components.

Third-party components:

| Component | Version | License | Used for |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) | 7.1.5 | LGPL-2.1-or-later | encoding and muxing, statically linked |
| [libvpl](https://github.com/intel/libvpl) | v2.16.0 | MIT | Intel Quick Sync dispatcher, statically linked |
| [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) | n12.1.14.0 | MIT | NVENC headers only |
| [AMF](https://github.com/GPUOpen-LibrariesAndSDKs/AMF) | v1.4.36 | MIT | AMD AMF headers only |

Their license texts are in [licenses/](licenses/).
The GPU encoder runtimes come with the graphics drivers and are not part of this DLL.

The FFmpeg source archive used for each release is attached to that release on GitHub.

## What it does

- **Video:** BGRA frames are converted to NV12 (BT.709, limited range) and encoded as H.264.
  - The first encoder that opens is used, in this order: `h264_nvenc`, `h264_qsv`, `h264_amf`, `h264_mf` (hardware), then `h264_mf` (software).
- **Audio:** f32, s16 or s32 PCM is resampled to planar float and encoded as AAC at 192 kbps.
- **Container:** fragmented MP4.

Exported functions: `kff_set_log`, `kff_check`, `kff_open`, `kff_video`, `kff_audio`, `kff_finish`, `kff_close`, `kff_strerror`.

## Building

Requirements:

- Visual Studio Build Tools with the x64 C++ tools and CMake.
- [MSYS2](https://www.msys2.org) with these packages: `pacman -S make diffutils nasm pkgconf`.
- `git` and `curl` on `PATH`.

Run:

```
build.bat
```

The script downloads the sources listed above into `%FF_WORK%\src`, builds FFmpeg and libvpl as static libraries with the static CRT (`-MT`), and links them with `kff.c` into `Kalmuri.dll`.

You can change the paths with these environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `VCVARS` | `D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat` | MSVC environment script |
| `MSYS2` | `D:\Vendor\msys64` | MSYS2 install folder |
| `FF_WORK` | `D:\Vendor\FFMpeg\build` | work folder for sources and build files |
| `FF_DLL` | `D:\Vendor\Kalmuri` | output folder for `Kalmuri.dll` |

The import library `Kalmuri.lib` and the configure flags actually used (`CONFIGURE.txt`) are written to `%FF_WORK%\codec`.

### FFmpeg configure flags

```
--toolchain=msvc --arch=x86_64 --target-os=win64
--enable-static --disable-shared
--disable-everything --disable-autodetect
--disable-programs --disable-doc --disable-network
--disable-avdevice --disable-avfilter --disable-postproc
--enable-swscale --enable-swresample
--enable-encoder=h264_nvenc,h264_qsv,h264_amf,h264_mf,aac
--enable-muxer=mp4 --enable-protocol=file
--enable-ffnvcodec --enable-nvenc --enable-libvpl --enable-amf --enable-mediafoundation
--pkg-config-flags=--static
```

## Replacing the DLL in Kalmuri

1. Build `Kalmuri.dll` as described above.
2. Close Kalmuri.
3. Copy the new DLL over `Kalmuri.dll` in the Kalmuri install folder.

Keep the `kff_*` function names and signatures unchanged, because Kalmuri calls them directly.
Kalmuri does not use the DLL if `kff_check` returns 0. `kff_check` returns 0 when the FFmpeg libraries linked into the DLL have different major versions from the headers `kff.c` was compiled with.
