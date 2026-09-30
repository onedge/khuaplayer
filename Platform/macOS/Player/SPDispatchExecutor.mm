#import "SPDispatchExecutor.h"

#import <Foundation/Foundation.h>

#include "Player/SPExecutor.hpp"

#include <utility>

namespace {

// Exactly the calls the media core made before: dispatch_async and
// dispatch_after on the main queue, so ordering and timing are unchanged.
class DispatchMainThreadExecutor final : public sp::MainThreadExecutor {
public:
    void post(std::function<void()> task) override {
        dispatch_async(dispatch_get_main_queue(), ^{ task(); });
    }

    void postAfter(int64_t delayUs, std::function<void()> task) override {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delayUs * (int64_t)NSEC_PER_USEC),
                       dispatch_get_main_queue(), ^{ task(); });
    }

    bool isCurrent() const override { return [NSThread isMainThread]; }
};

} // namespace

void SPInstallDispatchMainThreadExecutor(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!sp::mainThreadExecutorInstalled()) {
            sp::installMainThreadExecutor(std::make_unique<DispatchMainThreadExecutor>());
        }
    });
}
