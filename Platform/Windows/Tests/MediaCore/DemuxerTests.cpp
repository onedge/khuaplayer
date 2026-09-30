#include "Demuxer.hpp"
#include "Platform/SPFileSystem.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kRate = 48000;
constexpr int kSeconds = 1;

void put16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
}

void put32(std::vector<uint8_t> &out, uint32_t v) {
    put16(out, (uint16_t)v);
    put16(out, (uint16_t)(v >> 16));
}

// 16-bit mono PCM. A square wave rather than silence, so resilience checks do
// not mistake all-zero data for a damaged region.
std::vector<uint8_t> makeWav() {
    const uint32_t samples = kRate * kSeconds;
    const uint32_t dataBytes = samples * 2;
    std::vector<uint8_t> wav;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(wav, 36 + dataBytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(wav, 16);
    put16(wav, 1); // PCM
    put16(wav, 1); // mono
    put32(wav, kRate);
    put32(wav, kRate * 2);
    put16(wav, 2);
    put16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(wav, dataBytes);
    for (uint32_t i = 0; i < samples; ++i) put16(wav, (i / 50) % 2 ? 0x2000 : 0xE000);
    return wav;
}

std::string utf8(const fs::path &path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

int currentProcessId() {
#if defined(_WIN32)
    return _getpid();
#else
    return (int)getpid();
#endif
}

class DemuxerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path name(u8"khua-demux-");
        name += std::to_string(currentProcessId()) + "-" + std::to_string(stamp);
        dir_ = fs::temp_directory_path() / name;
        fs::create_directories(dir_);
        path_ = dir_ / fs::path(u8"테스트 음성.wav");
        const std::vector<uint8_t> wav = makeWav();
        std::ofstream(path_, std::ios::binary).write((const char *)wav.data(), (std::streamsize)wav.size());
        dataBytes_ = (int64_t)wav.size() - 44;
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }

    fs::path dir_;
    fs::path path_;
    int64_t dataBytes_ = 0;
};

} // namespace

// The whole Windows file path end to end: UTF-8 name outside the ANSI code
// page, spfs handle, custom AVIO, FFmpeg's wav demuxer and packet reads.
TEST_F(DemuxerTest, OpensAndReadsAWavWithANonAsciiName) {
    sp::Demuxer demuxer;
    ASSERT_EQ(demuxer.open(utf8(path_)), 0);
    EXPECT_EQ(demuxer.containerName(), "wav");
    ASSERT_GE(demuxer.audioStream(), 0);
    EXPECT_LT(demuxer.videoStream(), 0);
    EXPECT_NEAR((double)demuxer.durationUs(), kSeconds * 1e6, 50'000);
    EXPECT_EQ(demuxer.fileSizeBytes(), dataBytes_ + 44);
    EXPECT_FALSE(demuxer.onRemoteVolume());

    // readPacket returns > 0 for a packet, 0 at end of file and < 0 on error.
    AVPacket *pkt = av_packet_alloc();
    int64_t bytes = 0;
    int packets = 0;
    int ret = 0;
    while ((ret = demuxer.readPacket(pkt)) > 0) {
        if (pkt->stream_index == demuxer.audioStream()) {
            bytes += pkt->size;
            ++packets;
        }
        av_packet_unref(pkt);
        ASSERT_LT(packets, 100000) << "demuxer never reached end of file";
    }
    EXPECT_EQ(ret, 0);
    EXPECT_GT(packets, 0);
    EXPECT_EQ(bytes, dataBytes_);
    av_packet_free(&pkt);
    demuxer.close();
}

TEST_F(DemuxerTest, SeeksAndKeepsReading) {
    sp::Demuxer demuxer;
    ASSERT_EQ(demuxer.open(utf8(path_)), 0);
    EXPECT_GE(demuxer.seekToUs(500'000), 0);
    AVPacket *pkt = av_packet_alloc();
    ASSERT_GT(demuxer.readPacket(pkt), 0);
    // A packet near the middle of the 96 000-byte data chunk.
    EXPECT_GT(pkt->pos, 44 + dataBytes_ / 4);
    av_packet_free(&pkt);
}

TEST_F(DemuxerTest, SourceIdentityMatchesTheFileOnDisk) {
    sp::Demuxer demuxer;
    ASSERT_EQ(demuxer.open(utf8(path_)), 0);
    uint64_t dev = 0, ino = 0;
    int64_t size = 0, mtimeNs = 0;
    ASSERT_TRUE(demuxer.openSourceIdentity(dev, ino, size, mtimeNs));
    spfs::FileStat st;
    ASSERT_TRUE(spfs::statPath(utf8(path_), &st));
    EXPECT_EQ(dev, st.identity.volume);
    EXPECT_EQ(ino, st.identity.fileLow);
    EXPECT_EQ(size, st.size);
    EXPECT_EQ(mtimeNs, st.mtimeNs);
}

TEST_F(DemuxerTest, MissingFileFailsToOpen) {
    sp::Demuxer demuxer;
    EXPECT_LT(demuxer.open(utf8(dir_ / "missing.wav")), 0);
}

// The open file can still be renamed and deleted, as a downloader finishing
// the file or the user tidying up would do.
TEST_F(DemuxerTest, OpenFileCanBeRenamedAndDeleted) {
    sp::Demuxer demuxer;
    ASSERT_EQ(demuxer.open(utf8(path_)), 0);
    const fs::path renamed = dir_ / fs::path(u8"이름 바꿈.wav");
    std::error_code error;
    fs::rename(path_, renamed, error);
    EXPECT_FALSE(error) << error.message();
    fs::remove(renamed, error);
    EXPECT_FALSE(error) << error.message();
    demuxer.close();
}
