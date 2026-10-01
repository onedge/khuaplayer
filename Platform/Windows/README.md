# Windows port — work in progress

Khua for Windows is planned as a WinUI 3 (C#) app over a C++ media-core DLL,
x64 only. Nothing here produces a runnable app yet.

## Relationship to the Mac app

The Windows port owns its code. `MediaCore/` started as a copy of the portable
parts of `Modules/MediaCore` (the demuxer, recovery and resilience logic,
playback policies, and the file, thread and executor layers written for the
port) and is maintained here independently. The Windows build never compiles
anything under `Modules/`, and changes for Windows never touch the Mac app, so
they need no Mac build or playback check.

The Mac player (`Modules/MediaCore/Bridge/SPPlayerCore.mm` and
`Platform/macOS`) is the reference for behaviour: the Windows player core is
written from it in C++, against the interfaces in `MediaCore/Core/Player`. A
fix made on one side is ported to the other by hand.

## Layout

| Path | Contents |
|---|---|
| `MediaCore/Core` | Demuxer, recovery, resilience, seek and source-growth logic; `Platform/` (file access and threads on Win32); `Player/` (executor, background tasks, frame handle, listener, audio sink) |
| `MediaCore/Bridge` | Portable playback policies and helpers: frame selection, time stretch, subtitle compositing, color metadata, channel mapping, Dolby Vision RPU |
| `Tests/MediaCore` | GoogleTest suites and the header check |
| `vcpkg/ports/ffmpeg` | FFmpeg overlay port |

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

The shaders are compiled by `fxc`, which the developer prompt puts on `PATH`.
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

## Status

| Area | State |
|---|---|
| Headers | Every `Core/` and `Bridge/` header compiles on its own (`khua_header_check`) |
| `Core/Demuxer.cpp` | Builds; all OS access goes through `Core/Platform`. `DemuxerTests` open, read and seek a WAV with a Korean name, and rename and delete it while open |
| `Core/Platform` | Win32 file handles (overlapped, fully shared so downloads continue and can be renamed), positional reads, stat, current path, local/remote volume, sparse ranges, writer detection, thread names and priorities, and cancelling a blocked read with `CancelIoEx`/`CancelSynchronousIo`. Errors are errno values. Paths beyond `MAX_PATH` need the app's `longPathAware` manifest setting |
| `Core/Player` | UI-thread executor, background tasks (std::thread), `WaitGroup`/`Semaphore`, `VideoFrameRef`, `PlayerListener`/`PlayerError`, and `AudioSink`/`AudioSinkRef`, and `AVFrame`-backed frame handles: the interfaces the Windows player core is written against |
| Player core | Not yet: to be written in C++ from `SPPlayerCore.mm` |
| `Core/Audio` | `AudioEngine`, the logic of the Mac `SPAudioOutput.mm` without the AudioUnit: 48 kHz ring, epochs, pitch-preserving rate changes that regenerate queued audio, gain ramp, limiter and the media clock |
| Output | WASAPI audio sink (shared mode, event driven, 5.1/7.1 from the endpoint's mix format, follows default-device changes); the clock counts only what the endpoint has played and holds across pause. D3D11 renderer core: the Mac renderer's submission state machine, output-mode policy and colour pipeline on a submit thread, HLSL shaders compiled by fxc, software NV12/P010/yuv420p(10) uploads and D3D11VA texture-array slices, subtitle overlay and Dolby Vision RPU queue; `RendererTests` check pixels against a CPU reference through an offscreen target. Not yet: swap chain and display HDR detection, D3D11VA decoding |
| App | Not yet: C ABI DLL and WinUI 3 app |
