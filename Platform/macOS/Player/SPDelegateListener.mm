#import "SPDelegateListener.h"

#import "SPPlayerCore.h"

static_assert((int)sp::PlayerState::Idle == SPPlayerStateIdle);
static_assert((int)sp::PlayerState::Opening == SPPlayerStateOpening);
static_assert((int)sp::PlayerState::Ready == SPPlayerStateReady);
static_assert((int)sp::PlayerState::Playing == SPPlayerStatePlaying);
static_assert((int)sp::PlayerState::Paused == SPPlayerStatePaused);
static_assert((int)sp::PlayerState::Ended == SPPlayerStateEnded);
static_assert((int)sp::PlayerState::Failed == SPPlayerStateFailed);

namespace {

NSString *spString(const std::string &utf8) {
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding] ?: @"";
}

class DelegateListener final : public sp::PlayerListener {
public:
    explicit DelegateListener(SPPlayerCore *core) : core_(core) {}

    void didChangeState(sp::PlayerState state) override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCore:didChangeState:)])
            [d playerCore:core_ didChangeState:(SPPlayerState)state];
    }

    void didUpdatePosition(double positionSec, double durationSec) override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCore:didUpdatePosition:duration:)])
            [d playerCore:core_ didUpdatePosition:positionSec duration:durationSec];
    }

    // The same NSError that -makeErrorWithDomain:... built.
    void didFail(const sp::PlayerError &error) override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if (![d respondsToSelector:@selector(playerCore:didFailWithError:)]) return;
        NSError *err = [NSError errorWithDomain:spString(error.domain)
                                           code:(NSInteger)error.code
                                       userInfo:@{NSLocalizedDescriptionKey : spString(error.description),
                                                  SPPlayerErrorPhaseKey : spString(error.phase),
                                                  SPPlayerErrorDiagnosisKey : spString(error.diagnosis),
                                                  SPPlayerErrorTerminalKey : @(error.terminal)}];
        [d playerCore:core_ didFailWithError:err];
    }

    void didChangeFrameInterpolation() override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCoreDidChangeFrameInterpolation:)])
            [d playerCoreDidChangeFrameInterpolation:core_];
    }

    void didUpdateTimelinePreview() override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCoreDidUpdateTimelinePreview:)])
            [d playerCoreDidUpdateTimelinePreview:core_];
    }

    void didChangeDecoder() override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCoreDidChangeDecoder:)])
            [d playerCoreDidChangeDecoder:core_];
    }

    void didChangeXDRAvailability() override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCoreDidChangeXDRAvailability:)])
            [d playerCoreDidChangeXDRAvailability:core_];
    }

    void didChangeSourceGrowth() override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCoreDidChangeSourceGrowth:)])
            [d playerCoreDidChangeSourceGrowth:core_];
    }

    void didSkipMissingContent(double fromSec, double toSec, bool afterSeek) override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCore:didSkipMissingContentFrom:to:afterSeek:)])
            [d playerCore:core_ didSkipMissingContentFrom:fromSec to:toSec afterSeek:afterSeek];
    }

    void waitedSourceBecameReady() override {
        id<SPPlayerCoreDelegate> d = core_.delegate;
        if ([d respondsToSelector:@selector(playerCoreWaitedSourceBecameReady:)])
            [d playerCoreWaitedSourceBecameReady:core_];
    }

private:
    __unsafe_unretained SPPlayerCore *core_;
};

} // namespace

std::unique_ptr<sp::PlayerListener> SPMakeDelegateListener(SPPlayerCore *core) {
    return std::make_unique<DelegateListener>(core);
}
