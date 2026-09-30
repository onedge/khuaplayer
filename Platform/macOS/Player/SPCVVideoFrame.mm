// sp::VideoFrameRef operations for CVPixelBuffer: the same retain, release and
// getter calls the player core made on CVPixelBufferRef directly.
#define SP_VIDEO_FRAME_CV_BRIDGE 1
#include "Player/SPVideoFrame.hpp"

#import <CoreVideo/CoreVideo.h>

namespace {

void cvRetain(void *native) { CVPixelBufferRetain((CVPixelBufferRef)native); }
void cvRelease(void *native) { CVPixelBufferRelease((CVPixelBufferRef)native); }
size_t cvDataSize(void *native) { return CVPixelBufferGetDataSize((CVPixelBufferRef)native); }
int cvWidth(void *native) { return (int)CVPixelBufferGetWidth((CVPixelBufferRef)native); }
int cvHeight(void *native) { return (int)CVPixelBufferGetHeight((CVPixelBufferRef)native); }

} // namespace

namespace sp {

const VideoFrameOps kSPCVVideoFrameOps = {cvRetain, cvRelease, cvDataSize, cvWidth, cvHeight};

} // namespace sp
