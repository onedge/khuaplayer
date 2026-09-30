extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/hwcontext.h>
}

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::vector<std::string> protocols(int output) {
    std::vector<std::string> names;
    void *opaque = nullptr;
    while (const char *name = avio_enum_protocols(&opaque, output)) {
        names.emplace_back(name);
    }
    return names;
}

bool supportsD3D11VA(const char *decoderName) {
    const AVCodec *codec = avcodec_find_decoder_by_name(decoderName);
    if (!codec) return false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config) return false;
        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return true;
        }
    }
}

} // namespace

// PRIVACY.md promises that playback makes no network requests. The Mac build
// guarantees it with --disable-network --enable-protocol=file; the Windows
// overlay port must do the same.
TEST(FFmpegConfiguration, OnlyTheFileProtocolIsBuiltIn) {
    EXPECT_EQ(protocols(0), std::vector<std::string>{"file"});
    EXPECT_EQ(protocols(1), std::vector<std::string>{"file"});
}

TEST(FFmpegConfiguration, WhitelistedComponentsArePresent) {
    for (const char *name : {"h264", "hevc", "vp9", "libdav1d", "av1", "aac", "libspeex", "subrip", "ass"}) {
        EXPECT_NE(avcodec_find_decoder_by_name(name), nullptr) << name;
    }
    for (const char *name : {"mov", "matroska", "mpegts", "avi"}) {
        EXPECT_NE(av_find_input_format(name), nullptr) << name;
    }
}

TEST(FFmpegConfiguration, ComponentsOutsideTheWhitelistAreAbsent) {
    EXPECT_EQ(avcodec_find_decoder_by_name("png"), nullptr);
    EXPECT_EQ(av_find_input_format("image2"), nullptr);
    // --disable-everything also removes every encoder and muxer.
    void *opaque = nullptr;
    int encoders = 0;
    while (const AVCodec *codec = av_codec_iterate(&opaque)) {
        if (av_codec_is_encoder(codec)) ++encoders;
    }
    EXPECT_EQ(encoders, 0);
}

TEST(FFmpegConfiguration, HardwareDecodersOfferD3D11VA) {
    for (const char *name : {"h264", "hevc", "vp9", "av1", "mpeg2video", "vc1", "wmv3"}) {
        EXPECT_TRUE(supportsD3D11VA(name)) << name;
    }
}
