// errno values that the media core uses as FFmpeg error codes (AVERROR(x))
// but that the Microsoft C runtime does not define. They only need to differ
// from the CRT's own values, which end at EWOULDBLOCK (140); they never reach
// the OS. Do not reuse the Linux numbers: ESTALE is 116 there, which is
// ENETDOWN in the Microsoft CRT.
#pragma once

#include <cerrno>

#if !defined(ESTALE)
#define ESTALE 170
#endif
