# FFmpeg component whitelist for Khua on Windows.
#
# The KHUA_FFMPEG_* lists must equal the variables of the same name without the
# prefix in Scripts/build_ffmpeg_min.sh; the ffmpeg_overlay_matches_mac test
# (Platform/Windows/Tests/MediaCore/CheckFFmpegOverlay.cmake) enforces this.
# The lists live in the port directory because vcpkg's binary cache only
# hashes files here.
set(KHUA_FFMPEG_DEMUXERS mov,matroska,mpegts,mpegps,avi,asf,flv,ogg,wav,mp3,flac,aac,ac3,eac3,dts,m4v,h264,hevc,ivf,mpegvideo,srt,ass,webvtt,rm,wv,ape,tta,aiff,amr,mxf,av1,obu,dv,vvc,cavsvideo,dnxhd)
set(KHUA_FFMPEG_VDEC h264,hevc,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,wmv3,vc1,vp8,vp9,prores,mjpeg,theora,flv,h263,rawvideo,libdav1d,rv30,rv40,mpeg1video,vp6,vp6f,vp6a,rv10,rv20,svq1,svq3,dvvideo,huffyuv,utvideo,vvc,cavs,dnxhd,jpeg2000,ffv1)
set(KHUA_FFMPEG_ADEC aac,aac_latm,ac3,eac3,mp3,mp2,mp1,opus,vorbis,flac,alac,dca,truehd,mlp,wmav1,wmav2,wmapro,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_f32le,pcm_f64le,pcm_u8,pcm_alaw,pcm_mulaw,adpcm_ima_wav,adpcm_ms,cook,sipr,ra_144,ra_288,pcm_bluray,pcm_dvd,wavpack,ape,tta,amrnb,amrwb,libspeex,wmalossless)
set(KHUA_FFMPEG_SDEC subrip,ass,ssa,webvtt,mov_text,text)
set(KHUA_FFMPEG_PARSERS h264,hevc,mpeg4video,mpegvideo,mjpeg,prores,vp8,vp9,av1,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca,mlp,vvc,cavsvideo,dnxhd,jpeg2000,ffv1)
set(KHUA_FFMPEG_BSFS extract_extradata,av1_frame_merge)

# Windows-only additions. The Mac decodes through VideoToolbox directly and
# builds with --disable-hwaccels; Windows decodes on the GPU through FFmpeg's
# D3D11VA hwaccels. FFmpeg's hardware AV1 path requires its native av1
# decoder; libdav1d remains the software AV1 decoder, as on the Mac.
set(KHUA_FFMPEG_WINDOWS_VDEC av1)
set(KHUA_FFMPEG_WINDOWS_HWACCELS av1_d3d11va,av1_d3d11va2,h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2,mpeg2_d3d11va,mpeg2_d3d11va2,vc1_d3d11va,vc1_d3d11va2,vp9_d3d11va,vp9_d3d11va2,wmv3_d3d11va,wmv3_d3d11va2)

string(JOIN " " KHUA_FFMPEG_OPTIONS
    --disable-everything
    --enable-demuxer=${KHUA_FFMPEG_DEMUXERS}
    --enable-decoder=${KHUA_FFMPEG_VDEC},${KHUA_FFMPEG_WINDOWS_VDEC},${KHUA_FFMPEG_ADEC},${KHUA_FFMPEG_SDEC}
    --enable-parser=${KHUA_FFMPEG_PARSERS}
    --enable-bsf=${KHUA_FFMPEG_BSFS}
    --enable-hwaccel=${KHUA_FFMPEG_WINDOWS_HWACCELS}
    --enable-protocol=file
)
