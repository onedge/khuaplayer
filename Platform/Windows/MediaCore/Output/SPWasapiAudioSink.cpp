#include "SPWasapiAudioSink.hpp"

#include "Audio/SPAudioEngine.hpp"
#include "Platform/SPThread.hpp"
#include "SPAudioChannelMap.hpp"
#include "SPRuntimeGates.hpp"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sp {
namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid ksmedia.h's GUID storage.
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr REFERENCE_TIME kBufferDuration = 300000; // 30 ms in 100 ns units
constexpr int kRate = AudioEngine::kSampleRate;

static_assert(kSPChFL == SPEAKER_FRONT_LEFT && kSPChFR == SPEAKER_FRONT_RIGHT &&
              kSPChFC == SPEAKER_FRONT_CENTER && kSPChLFE == SPEAKER_LOW_FREQUENCY &&
              kSPChBL == SPEAKER_BACK_LEFT && kSPChBR == SPEAKER_BACK_RIGHT &&
              kSPChSL == SPEAKER_SIDE_LEFT && kSPChSR == SPEAKER_SIDE_RIGHT,
              "kSPCh* masks are the WAVEFORMATEXTENSIBLE speaker bits");

// Endpoint speaker positions, in channel order, as SPSpeakerLabel values.
std::vector<uint32_t> labelsForMask(DWORD mask) {
    std::vector<uint32_t> labels;
    for (DWORD bit = 1; bit != 0 && bit <= mask; bit <<= 1) {
        if (!(mask & bit)) continue;
        switch (bit) {
        case SPEAKER_FRONT_LEFT: labels.push_back(kSPSpeakerLeft); break;
        case SPEAKER_FRONT_RIGHT: labels.push_back(kSPSpeakerRight); break;
        case SPEAKER_FRONT_CENTER: labels.push_back(kSPSpeakerCenter); break;
        case SPEAKER_LOW_FREQUENCY: labels.push_back(kSPSpeakerLFE); break;
        case SPEAKER_BACK_LEFT: labels.push_back(kSPSpeakerRearSurroundLeft); break;
        case SPEAKER_BACK_RIGHT: labels.push_back(kSPSpeakerRearSurroundRight); break;
        case SPEAKER_SIDE_LEFT: labels.push_back(kSPSpeakerLeftSurroundDirect); break;
        case SPEAKER_SIDE_RIGHT: labels.push_back(kSPSpeakerRightSurroundDirect); break;
        default: labels.push_back(0); break; // a position the player never feeds
        }
    }
    return labels;
}

DWORD defaultMaskForChannels(int channels) {
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return KSAUDIO_SPEAKER_STEREO;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default: return 0;
    }
}

bool deviceLost(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

// a * b / c without overflowing for stream positions and QPC values.
int64_t mulDiv(uint64_t a, uint64_t b, uint64_t c) {
    return (int64_t)((a / c) * b + (a % c) * b / c);
}

// The layout-change handler's thread. The Mac output runs the handler on the
// main queue; here it gets a thread of its own so that it may call back into
// the sink and wait, and rendering never stops for it. The thread shares
// this state, so a handler that releases the sink's last reference does not
// make the thread join itself.
struct HandlerState {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> handler;
    bool pending = false;
    bool quit = false;
};

void handlerThread(std::shared_ptr<HandlerState> state) {
    spfs::setCurrentThreadName("sp.audio.layout");
    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        state->cv.wait(lock, [&] { return state->quit || state->pending; });
        if (state->quit) return;
        state->pending = false;
        std::function<void()> handler = state->handler;
        lock.unlock();
        if (handler) handler();
        handler = nullptr;
        lock.lock();
    }
}

// Forwards default-endpoint changes to the sink's device thread.
class EndpointNotifier final : public IMMNotificationClient {
public:
    explicit EndpointNotifier(std::function<void()> onDefaultRenderChanged)
        : onChange_(std::move(onDefaultRenderChanged)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) onChange_();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    std::function<void()> onChange_;
};

class WasapiAudioSink final : public AudioSink {
public:
    WasapiAudioSink() {
        commandEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = std::thread([this] { deviceThread(); });
        // Wait until the thread's COM objects exist, so commands can run.
        started_.get_future().wait();
        handlerThread_ = std::thread(handlerThread, handlerState_);
    }

    ~WasapiAudioSink() override {
        {
            std::lock_guard<std::mutex> lock(handlerState_->mutex);
            handlerState_->quit = true;
        }
        handlerState_->cv.notify_all();
        if (handlerThread_.get_id() == std::this_thread::get_id()) handlerThread_.detach();
        else if (handlerThread_.joinable()) handlerThread_.join();
        quit_.store(true);
        SetEvent(commandEvent_);
        if (thread_.joinable()) thread_.join();
        CloseHandle(commandEvent_);
        CloseHandle(renderEvent_);
    }

    // ---- AudioSink ------------------------------------------------------

    bool setup() override {
        return run<bool>([this] { return setupOnDevice(); });
    }

    void markSetupFailed() override { engine_.setDead(true); }

    uint64_t outputChannelMask() override {
        run<int>([this] {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            if (!negotiatedOnce_) negotiateLocked();
            return 0;
        });
        std::lock_guard<std::mutex> lock(layoutMutex_);
        return appliedOnce_ ? applied_.mask : negotiated_.mask;
    }

    int outputChannels() override { return engine_.channels(); }

    bool outputLayoutChangePending() override {
        std::lock_guard<std::mutex> lock(layoutMutex_);
        return layoutPending_;
    }

    bool applyPendingOutputLayout() override {
        return run<bool>([this] { return applyPendingLayoutOnDevice(); });
    }

    void setOutputLayoutChangeHandler(std::function<void()> handler) override {
        std::lock_guard<std::mutex> lock(handlerState_->mutex);
        handlerState_->handler = std::move(handler);
    }

    std::string outputLayoutDescription() override {
        std::lock_guard<std::mutex> lock(layoutMutex_);
        const AudioOutputLayout &layout = appliedOnce_ ? applied_ : negotiated_;
        return std::string(layout.name()) + "/" + std::to_string(layout.deviceChannels);
    }

    std::string outputLayoutName() override {
        std::lock_guard<std::mutex> lock(layoutMutex_);
        return (appliedOnce_ ? applied_ : negotiated_).name();
    }

    void start() override {
        if (engine_.dead()) return;
        desiredRunning_.store(true);
        startRequested_.store(true);
        if (!ready_.load()) return; // setup starts it
        if (startRequested_.exchange(false)) {
            run<int>([this] {
                startOnDevice();
                return 0;
            });
        }
    }

    void stop() override {
        desiredRunning_.store(false);
        startRequested_.store(false);
        if (ready_.load()) {
            run<int>([this] {
                stopOnDevice();
                return 0;
            });
        }
        engine_.setRunning(false);
    }

    void releaseSessionScratch() override { engine_.releaseSessionScratch(); }
    // A new epoch: what the endpoint holds belongs to the old one, and the
    // player takes its new clock base right after this, so flush it.
    void reset() override {
        engine_.reset();
        if (!ready_.load()) return;
        run<int>([this] {
            if (!client_) return 0;
            const bool wasRunning = engine_.running();
            if (wasRunning) {
                client_->Stop();
                engine_.setRunning(false);
            }
            client_->Reset();
            discardEndpointQueue();
            if (wasRunning) startOnDevice();
            return 0;
        });
    }
    void abortWrites() override { engine_.abortWrites(); }

    bool writePCM(const float *data, int frames, int channels, double rate, int32_t epoch) override {
        return engine_.writePCM(data, frames, channels, rate, epoch);
    }
    int32_t currentEpoch() override { return engine_.currentEpoch(); }
    void requestRate(double rate) override { engine_.requestRate(rate); }
    bool rateSwitchPending() override { return engine_.rateSwitchPending(); }
    int64_t rateSwitchPlayedFrame() override { return engine_.rateSwitchPlayedFrame(); }
    void servicePendingRateSwitch(int32_t epoch) override { engine_.servicePendingRateSwitch(epoch); }
    void drainStretchAtEOF(int32_t epoch) override { engine_.drainStretchAtEOF(epoch); }
    int64_t clockFrames() override { return engine_.clockFrames(); }
    int64_t bufferedFrames() override { return engine_.bufferedFrames(); }
    void setVolume(float volume) override { engine_.setVolume(volume); }
    bool isRunning() override { return engine_.running(); }

private:
    // ---- Device thread ---------------------------------------------------

    // Runs `work` on the device thread and waits for it. From the device
    // thread itself it runs inline.
    template <class R> R run(std::function<R()> work) {
        if (std::this_thread::get_id() == deviceThreadId_) return work();
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(work));
        std::future<R> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(commandMutex_);
            commands_.push_back([task] { (*task)(); });
        }
        SetEvent(commandEvent_);
        return result.get();
    }

    void post(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(commandMutex_);
            commands_.push_back(std::move(work));
        }
        SetEvent(commandEvent_);
    }

    void deviceThread() {
        deviceThreadId_ = std::this_thread::get_id();
        spfs::setCurrentThreadName("sp.audio.wasapi");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
        if (enumerator_) {
            notifier_ = new EndpointNotifier([this] { post([this] { handleDefaultDeviceChanged(); }); });
            if (FAILED(enumerator_->RegisterEndpointNotificationCallback(notifier_))) {
                notifier_->Release();
                notifier_ = nullptr;
            }
        }
        started_.set_value();

        while (!quit_.load()) {
            const HANDLE handles[2] = {commandEvent_, renderEvent_};
            const DWORD count = client_ ? 2 : 1;
            const DWORD woke = WaitForMultipleObjects(count, handles, FALSE, 200);
            if (woke == WAIT_OBJECT_0 || woke == WAIT_TIMEOUT) drainCommands();
            if (woke == WAIT_OBJECT_0 + 1 || woke == WAIT_TIMEOUT) fill();
            if (woke == WAIT_TIMEOUT) retryBrokenDevice();
        }

        drainCommands();
        if (client_) client_->Stop();
        releaseClient();
        if (notifier_) {
            enumerator_->UnregisterEndpointNotificationCallback(notifier_);
            notifier_->Release();
            notifier_ = nullptr;
        }
        enumerator_.Reset();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void drainCommands() {
        for (;;) {
            std::function<void()> work;
            {
                std::lock_guard<std::mutex> lock(commandMutex_);
                if (commands_.empty()) return;
                work = std::move(commands_.front());
                commands_.pop_front();
            }
            work();
        }
    }

    // Tops up the endpoint buffer while the stream runs, or before it starts.
    // Not while paused: the clock stays frozen at what was heard.
    void fill(bool starting = false) {
        if (!client_ || !render_) return;
        if (!starting && !engine_.running()) return;
        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (deviceLost(hr)) {
            rebuildOnDefaultDevice();
            return;
        }
        if (FAILED(hr) || padding >= bufferFrames_) return;
        const UINT32 frames = bufferFrames_ - padding;
        int64_t hostUs = spNowUs();
        int64_t queued = padding;
        devicePosition(&hostUs, &queued);
        BYTE *data = nullptr;
        hr = render_->GetBuffer(frames, &data);
        if (deviceLost(hr)) {
            rebuildOnDefaultDevice();
            return;
        }
        if (FAILED(hr)) return;
        if (clientChannels_ == engine_.channels()) {
            engine_.render(reinterpret_cast<float *>(data), (int)frames, hostUs, queued);
            render_->ReleaseBuffer(frames, 0);
        } else {
            // Between a layout change and its reset: never hand the device a
            // buffer rendered for another channel count.
            engine_.noteSilenceQueued((int)frames);
            render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        released_ += frames;
    }

    // Replaces the padding estimate with what the device is playing, dated
    // on the spNowUs() clock. Unlike the padding, this includes the latency
    // past the endpoint buffer.
    void devicePosition(int64_t *hostUs, int64_t *queued) {
        if (!audioClock_ || clockFrequency_ == 0) return;
        UINT64 position = 0, qpcPosition = 0;
        if (FAILED(audioClock_->GetPosition(&position, &qpcPosition))) return;
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const int64_t now100ns = mulDiv((uint64_t)now.QuadPart, 10000000, (uint64_t)frequency.QuadPart);
        const int64_t ageUs = std::max<int64_t>(0, now100ns - (int64_t)qpcPosition) / 10;
        const int64_t played = mulDiv(position, kRate, clockFrequency_);
        *hostUs = spNowUs() - ageUs;
        *queued = std::max<int64_t>(0, released_ - played);
    }

    // A lost device that could not be reopened (no endpoint, or another app
    // holds it exclusively) is retried every second, not only when the
    // default endpoint changes.
    void retryBrokenDevice() {
        bool broken = false;
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            broken = clientBroken_;
        }
        if (!broken || !ready_.load()) return;
        const int64_t now = spNowUs();
        if (now - lastRetryUs_ < 1000000) return;
        lastRetryUs_ = now;
        rebuildOnDefaultDevice();
    }

    // Reads the default endpoint's mix format and picks stereo, 5.1 or 7.1.
    void negotiateLocked() {
        AudioOutputLayout layout;
        initStereo(&layout, 0);
        ComPtr<IMMDevice> device;
        if (enumerator_ && SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            ComPtr<IAudioClient> probe;
            WAVEFORMATEX *mix = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe)) &&
                SUCCEEDED(probe->GetMixFormat(&mix)) && mix) {
                const int channels = mix->nChannels;
                DWORD mask = defaultMaskForChannels(channels);
                if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
                    mask = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->dwChannelMask;
                CoTaskMemFree(mix);
                const std::vector<uint32_t> labels = labelsForMask(mask);
                if (channels >= 6 && (int)labels.size() == channels) {
                    resolveOutputLayout(labels.data(), (int)labels.size(), &layout);
                } else {
                    initStereo(&layout, channels);
                }
            }
        }
        negotiated_ = layout;
        negotiatedOnce_ = true;
        if (!appliedOnce_) {
            engine_.setChannelsBeforeFirstUse(layout.channels);
            layoutPending_ = false;
        } else {
            layoutPending_ = (layout != applied_) || clientBroken_;
        }
    }

    void releaseClient() {
        audioClock_.Reset();
        render_.Reset();
        client_.Reset();
        clientChannels_ = 0;
        bufferFrames_ = 0;
        clockFrequency_ = 0;
        released_ = 0;
        boundDeviceId_.clear();
    }

    std::wstring defaultDeviceId() {
        ComPtr<IMMDevice> device;
        LPWSTR id = nullptr;
        if (!enumerator_ || FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->GetId(&id)))
            return {};
        std::wstring result(id);
        CoTaskMemFree(id);
        return result;
    }

    // Opens a shared-mode stream for `layout` on the default endpoint; on
    // failure retries in stereo, as the Mac output does, and updates `layout`.
    bool createClient(AudioOutputLayout &layout) {
        releaseClient();
        if (!enumerator_) return false;
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt == 1) {
                if (layout.channels == 2) break;
                initStereo(&layout, layout.deviceChannels);
            }
            ComPtr<IAudioClient> client;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
            WAVEFORMATEXTENSIBLE format = {};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = (WORD)layout.channels;
            format.Format.nSamplesPerSec = kRate;
            format.Format.wBitsPerSample = 32;
            format.Format.nBlockAlign = (WORD)(layout.channels * sizeof(float));
            format.Format.nAvgBytesPerSec = kRate * format.Format.nBlockAlign;
            format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = (DWORD)layout.mask;
            format.SubFormat = kSubtypeFloat;
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0,
                                          &format.Format, nullptr)))
                continue;
            UINT32 bufferFrames = 0;
            ComPtr<IAudioRenderClient> render;
            if (FAILED(client->SetEventHandle(renderEvent_)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
                FAILED(client->GetService(IID_PPV_ARGS(&render))))
                continue;
            client_ = client;
            render_ = render;
            bufferFrames_ = bufferFrames;
            clientChannels_ = layout.channels;
            if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&audioClock_))) &&
                FAILED(audioClock_->GetFrequency(&clockFrequency_)))
                clockFrequency_ = 0;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id))) {
                boundDeviceId_ = id;
                CoTaskMemFree(id);
            }
            return true;
        }
        return false;
    }

    void startOnDevice() {
        if (!client_ || engine_.running()) return;
        // A fresh fade, unless the endpoint still holds what a pause kept:
        // that resumes where it stopped, at the gain it had.
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding)) || padding == 0) engine_.noteDeviceStarting();
        // Fill the buffer before starting so the first period does not underrun.
        fill(true);
        // Losing the device in the fill has rebuilt and restarted the stream,
        // or left none.
        if (!client_ || engine_.running()) return;
        if (SUCCEEDED(client_->Start())) engine_.setRunning(true);
    }

    // Stops the stream but keeps what the endpoint holds: a resume plays it
    // first, so pausing neither skips audio nor moves the clock.
    void stopOnDevice() {
        if (client_) client_->Stop();
        engine_.freezeClock(spNowUs());
        engine_.setRunning(false);
    }

    // The endpoint's queued frames are gone (a flush, a new client): take
    // them out of the played count so the clock continues from what was heard.
    void discardEndpointQueue() {
        engine_.discardDeviceQueue(spNowUs());
        released_ = 0;
    }

    bool setupOnDevice() {
        AudioOutputLayout layout;
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            if (!negotiatedOnce_) negotiateLocked();
            layout = negotiated_;
        }
        if (!createClient(layout)) return false;
        bool pendingAfterSetup = false;
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            if (layout.channels != engine_.channels()) engine_.reset(layout.channels);
            applied_ = layout;
            appliedOnce_ = true;
            negotiateLocked();
            pendingAfterSetup = layoutPending_;
        }
        if (spDebug()) {
            std::fprintf(stderr, "[Audio] output layout: %s (%d channels -> endpoint %d), buffer %u frames\n",
                         layout.name(), layout.channels, layout.deviceChannels, bufferFrames_);
        }
        ready_.store(true);
        if (startRequested_.exchange(false)) {
            startOnDevice();
            if (!desiredRunning_.load() && engine_.running()) stopOnDevice();
        }
        if (pendingAfterSetup) post([this] { handleDefaultDeviceChanged(); });
        return true;
    }

    // The default endpoint changed. With the same layout the stream follows
    // it here; a different layout goes to the player through the handler.
    void handleDefaultDeviceChanged() {
        bool pending = false;
        const char *name = nullptr;
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            negotiateLocked();
            pending = layoutPending_;
            name = negotiated_.name(); // a string literal
        }
        if (spDebug())
            std::fprintf(stderr, "[Audio] default endpoint changed: %s%s\n", name,
                         pending ? " -> reconfigure" : " -> same layout");
        if (pending) {
            bool haveHandler = false;
            {
                std::lock_guard<std::mutex> lock(handlerState_->mutex);
                haveHandler = static_cast<bool>(handlerState_->handler);
                handlerState_->pending = haveHandler;
            }
            if (haveHandler) handlerState_->cv.notify_one();
            else applyPendingLayoutOnDevice();
        } else if (ready_.load()) {
            // A lost stream was already reopened on this endpoint.
            if (client_ && !boundDeviceId_.empty() && defaultDeviceId() == boundDeviceId_) return;
            rebuildOnDefaultDevice();
        }
    }

    // Reopens the stream with the applied layout on the current default
    // endpoint, keeping the engine's queued audio and clock.
    void rebuildOnDefaultDevice() {
        const bool wasRunning = engine_.running();
        if (client_) client_->Stop();
        engine_.setRunning(false);
        discardEndpointQueue();
        AudioOutputLayout layout;
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            layout = applied_;
        }
        const int channelsBefore = layout.channels;
        const bool ok = createClient(layout);
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            const bool wasBroken = clientBroken_;
            clientBroken_ = !ok;
            if (ok && layout.channels != channelsBefore) {
                // Fell back to stereo on the new endpoint.
                engine_.reset(layout.channels);
                applied_ = layout;
            }
            if (ok && wasBroken && negotiatedOnce_) layoutPending_ = layout != negotiated_;
        }
        engine_.setDead(!ok);
        if (ok && (wasRunning || desiredRunning_.load())) startOnDevice();
    }

    bool applyPendingLayoutOnDevice() {
        AudioOutputLayout layout;
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            if (!layoutPending_) return false;
            layout = negotiated_;
        }
        const bool haveClient = ready_.load();
        bool ok = true;
        if (haveClient) {
            if (client_) client_->Stop();
            engine_.setRunning(false);
            discardEndpointQueue();
            ok = createClient(layout);
            engine_.setDead(!ok);
            if (!ok) std::fprintf(stderr, "[Audio] reconfiguring the output failed; audio is muted until the next device change\n");
        }
        engine_.reset(layout.channels);
        {
            std::lock_guard<std::mutex> lock(layoutMutex_);
            clientBroken_ = haveClient && !ok;
            applied_ = layout;
            appliedOnce_ = true;
            layoutPending_ = (layout != negotiated_) || clientBroken_;
        }
        if (spDebug())
            std::fprintf(stderr, "[Audio] output layout reconfigured: %s (%d channels -> endpoint %d)\n",
                         layout.name(), layout.channels, layout.deviceChannels);
        if (haveClient && ok && desiredRunning_.load() && !engine_.running()) startOnDevice();
        return true;
    }

    AudioEngine engine_;

    std::thread thread_;
    std::thread::id deviceThreadId_;
    std::promise<void> started_;
    std::atomic<bool> quit_{false};
    HANDLE commandEvent_ = nullptr;
    HANDLE renderEvent_ = nullptr;
    std::mutex commandMutex_;
    std::deque<std::function<void()>> commands_;

    // Device-thread COM objects.
    ComPtr<IMMDeviceEnumerator> enumerator_;
    EndpointNotifier *notifier_ = nullptr;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    ComPtr<IAudioClock> audioClock_;
    UINT64 clockFrequency_ = 0;
    UINT32 bufferFrames_ = 0;
    int clientChannels_ = 0;
    int64_t released_ = 0; // frames handed to the stream since it began or was reset
    std::wstring boundDeviceId_;
    int64_t lastRetryUs_ = 0;

    std::atomic<bool> ready_{false};
    std::atomic<bool> startRequested_{false};
    std::atomic<bool> desiredRunning_{false};

    std::mutex layoutMutex_;
    AudioOutputLayout negotiated_;
    AudioOutputLayout applied_;
    bool negotiatedOnce_ = false;
    bool appliedOnce_ = false;
    bool layoutPending_ = false;
    bool clientBroken_ = false;

    std::shared_ptr<HandlerState> handlerState_ = std::make_shared<HandlerState>();
    std::thread handlerThread_;
};

} // namespace

std::shared_ptr<AudioSink> makeWasapiAudioSink() {
    return std::make_shared<WasapiAudioSink>();
}

} // namespace sp
