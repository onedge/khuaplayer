# Windows port — work in progress

Khua for Windows is planned as a WinUI 3 (C#) app over a C++ media-core DLL,
x64 only. Nothing here produces a runnable app yet. The Mac app remains the
shipping product and must keep building and playing at every commit.

## Current scope

The root `CMakeLists.txt` builds the platform-neutral media-core sources as
`khua_media_portable` and runs host-side unit tests. It does not replace the
Mac build (`Apps/Mac/Scripts/build.sh`).

## Prerequisites

- Visual Studio 2022 Build Tools with the C++ desktop workload, the
  "C++ Clang tools for Windows" component and the Windows 11 SDK
- CMake 3.25 or later and Ninja
- vcpkg, with `VCPKG_ROOT` pointing at it

## Build and test

From a "x64 Native Tools" developer prompt at the repository root:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug
```

The first configure builds FFmpeg and its dependencies through vcpkg, which
takes a while. Output goes to `.build/cmake/`, which Git ignores.

## Dependencies

`vcpkg.json` pins the versions in `ThirdParty/deps.lock.json`. FFmpeg 8.1.2
comes from the overlay port in `vcpkg/ports/ffmpeg`: the upstream vcpkg port
at that version plus the four patches in `Scripts/patches`, applied in the
order listed in `ThirdParty/deps.lock.json`. When a patch changes there, copy
it here as well. Only LGPL features are enabled.

Like the Mac build, the overlay configures FFmpeg with `--disable-network`, no
TLS backend and `--disable-everything`, then enables only the components that
`Scripts/build_ffmpeg_min.sh` lists, plus `--enable-protocol=file`; the lists
live in `vcpkg/ports/ffmpeg/khua-components.cmake`. Windows additionally
enables the D3D11VA hwaccels and FFmpeg's native `av1` decoder, which its
hardware AV1 path requires; the Mac decodes through VideoToolbox directly.

Two tests keep this honest. `ffmpeg_overlay_matches_mac` fails when the lists
or the patches differ from the Mac build, and the `FFmpegConfiguration` tests
inspect the built libraries: only the `file` protocol, no encoders, the
whitelisted decoders present and D3D11VA offered for hardware codecs.

vcpkg applies patches with `git apply`, which rejects hunks whose line counts
do not match. The overlay copy of `ffmpeg-mov-multistsd-seek.patch` therefore
restores the trailing blank context line (a single space) that the Mac copy
lacks; GNU `patch` on the Mac accepts either form. Keep that line when
re-copying the patch.

## Porting status

| Area | State |
|---|---|
| 55 `Core/` and `Bridge/` headers | Each compiles on its own under clang-cl (`khua_header_check`) |
| `TsRapScan.cpp`, `SPDoviRPU.cpp`, `SPFrameSelectionPolicy.cpp`, `SPSubtitleCompositor.cpp` | In `khua_media_portable` |
| `Core/Platform/SPFileSystem*`, `SPThread*` | File handles, positional reads, stat, current path, local/remote volume, sparse ranges, writer detection, thread names and priorities, disk I/O throttling, and interrupting a blocked read (SIGUSR2 on macOS, `CancelSynchronousIo` on Windows). Errors are errno values on both. Paths beyond `MAX_PATH` need the app's `longPathAware` manifest setting |
| `Core/Demuxer.cpp` | In `khua_media_portable`; all OS access goes through spfs. `DemuxerTests` open, read and seek a WAV with a Korean name. Windows handles are overlapped, so the reader, prefetch and scrub threads never queue behind one another, and an interrupt cancels the pending read with `CancelIoEx` |
| `SPColorMetadata.hpp`, `SPAudioChannelMap.hpp` | Portable. `SPVideoColorMetadata.hpp` keeps only the CoreVideo tag mapping on top of `SPColorMetadata.hpp`; channel labels are `SPSpeakerLabel`, whose values equal CoreAudio's |
| `SPPacketDataSnapshot.hpp`, `SPMotionFrameCompatibility.hpp` | Mac-only by design: an `NSData` view for Objective-C callers, and Motion+ `CVPixelBuffer` checks. The C++ core will get its own packet snapshot when it needs one |
| `Core/Player/SPExecutor`, `SPTasks`, `SPSync` | Process-wide UI-thread executor and background tasks (one-off tasks, serial queues, repeating timers), plus `WaitGroup`/`Semaphore`. `SPPlayerCore.mm` uses them instead of GCD (steps 1–2 of the core extraction); macOS installs GCD-backed implementations, Windows uses `SPThreadTasks.cpp` |
| `Core/Player/SPVideoFrame.hpp` | Opaque, reference-counted frame handle (`VideoFrameRef`, CF ownership rules). The core's frame state in `SPPlayerCore.mm` uses it (step 4); on macOS it wraps `CVPixelBuffer`. `SP_VIDEO_FRAME_CV_BRIDGE` lets the `.mm` mix it with `CVPixelBufferRef` until the core moves to C++; `VideoFrameBridgeTests` compiles those expressions on Windows |
| `SPPlayerCore.mm` and the other Objective-C++ bridge files | In progress: being split into a C++ core and platform adapters, one behaviour-preserving step at a time |
