// Standalone offline test. The implementation guard excludes ALL real miniaudio
// code; every backend symbol below is fake. Compile with AudioDevice.cpp only:
// c++ -std=c++17 -pthread -Iclient/afv-native/include \
//     client/afv-native/tests/device_lifecycle_tests.cpp \
//     client/afv-native/src/audio/AudioDevice.cpp -o /tmp/device_lifecycle
#define miniaudio_c
#include "../src/audio/MiniAudioAudioDevice.cpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

std::set<ma_context*> contexts;
ma_device* activeDevice = nullptr;
ma_device_data_proc callback = nullptr;
std::thread callbackThread;
bool stopped = true;
bool failContext = false;
bool failInit = false;
bool failStart = false;
unsigned int contextInits = 0;
unsigned int deviceInits = 0;
unsigned int deviceUninits = 0;
unsigned int starts = 0;
unsigned int stops = 0;
std::atomic<bool> stopEntered{false};

struct CallbackBarrier {
    void block()
    {
        entered = true;
        while (!released)
            std::this_thread::yield();
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
};

struct BlockingSource final : ISampleSource, CallbackBarrier {
    SourceStatus getAudioFrame(float* output) override
    {
        block();
        std::fill_n(output, frameSizeSamples * 2, 0.5f);
        return SourceStatus::OK;
    }
};

struct BlockingSink final : ISampleSink, CallbackBarrier {
    void putAudioFrame(const float*) override { block(); }
};

struct ConstantSource final : ISampleSource {
    explicit ConstantSource(float sample) : value(sample) {}
    SourceStatus getAudioFrame(float* output) override
    {
        std::fill_n(output, frameSizeSamples * 2, value);
        return SourceStatus::OK;
    }
    const float value;
};

struct ConstantSink final : ISampleSink {
    void putAudioFrame(const float* input) override
    {
        for (unsigned int i = 0; i < frameSizeSamples; ++i)
            check(input[i] == 0.75f, "sink has no previous-session tail");
        ++calls;
    }
    std::atomic<unsigned int> calls{0};
};

void render(float expected, unsigned int frames = 117)
{
    std::array<float, 4097 * 2 + 2> output;
    output.fill(99.0f);
    callback(activeDevice, output.data() + 1, nullptr, frames);
    const auto samples = frames * activeDevice->playback.channels;
    check(output[0] == 99.0f && output[samples + 1] == 99.0f, "device output guards");
    for (unsigned int i = 1; i <= samples; ++i)
        check(output[i] == expected, "device playback reset/silence");
}

void record(float value, unsigned int frames)
{
    std::array<float, 4097> input;
    input.fill(value);
    callback(activeDevice, nullptr, input.data(), frames);
}

void idleAndFailures()
{
    const auto originalContexts = contextInits;
    {
        MiniAudioAudioDevice idle("test", "", 0, false);
        idle.close();
        idle.close();
        check(!idle.openOutput() && !idle.openInput(), "empty device fails without backend");
    }
    check(contextInits == originalContexts, "construction/idle close never initializes backend");
    for (unsigned int failure = 0; failure < 3; ++failure) {
        const auto originalInits = deviceInits;
        const auto originalUninits = deviceUninits;
        {
            MiniAudioAudioDevice device("test", "fake", 0, true);
            failContext = failure == 0;
            failInit = failure == 1;
            failStart = failure == 2;
            check(!device.openOutput(), "injected open failure");
            check(activeDevice == nullptr, "failed start/init releases device");
            device.close();
            device.close();
            check(contexts.empty(), "close releases context after failure");
            failContext = failInit = failStart = false;
            check(device.openOutput(), "open recovers after failure");
            render(0.0f);
        }
        check(deviceInits - originalInits == deviceUninits - originalUninits,
              "exactly one uninit per successful init");
        check(contexts.empty() && activeDevice == nullptr, "destructor releases resources");
    }
    failContext = true;
    check(MiniAudioAudioDevice::getCompatibleInputDevices().empty(), "failed input enumeration");
    check(MiniAudioAudioDevice::getCompatibleOutputDevices().empty(), "failed output enumeration");
    failContext = false;
}

void restartAndEndpointTails()
{
    auto first = std::make_shared<ConstantSource>(0.25f);
    auto second = std::make_shared<ConstantSource>(0.75f);
    auto sink = std::make_shared<ConstantSink>();
    MiniAudioAudioDevice device("test", "fake", 0, true);
    device.setSource(first);
    check(device.openOutput(), "open playback");
    render(0.25f);
    const auto originalStarts = starts;
    check(device.openOutput() && starts == originalStarts, "open is idempotent while playing");
    device.setSource(second);
    render(0.75f, 961);
    device.setSource(nullptr);
    render(0.0f, 4097);
    device.setSource(first);
    render(0.25f);
    device.close();
    device.close();
    device.setSource(second);
    check(device.openOutput(), "reopen after close");
    render(0.75f);
    device.setSink(sink);
    check(device.openInput(), "switch playback to capture");
    record(0.25f, 117);
    device.close();
    check(device.openInput(), "reopen capture");
    record(0.75f, frameSizeSamples - 1);
    check(sink->calls == 0, "restart discards capture tail");
    record(0.75f, 1);
    check(sink->calls == 1, "restart delivers complete capture frame");
    record(0.25f, 117);
    device.setSink(nullptr);
    device.setSink(sink);
    record(0.75f, frameSizeSamples);
    check(sink->calls == 2, "reattach discards capture tail");
}

void concurrentEndpointsAndClose()
{
    auto first = std::make_shared<ConstantSource>(0.25f);
    auto second = std::make_shared<ConstantSource>(0.75f);
    auto sink = std::make_shared<ConstantSink>();
    {
        MiniAudioAudioDevice device("test", "fake", 0, true);
        check(device.openOutput(), "open concurrent playback");
        std::atomic<bool> entered{false};
        callbackThread = std::thread([&] {
            std::array<float, 961 * 2> output;
            entered = true;
            for (unsigned int i = 0; i < 2000; ++i) {
                callback(activeDevice, output.data(), nullptr, 961);
                for (float value : output)
                    check(value == 0.0f || value == 0.25f || value == 0.75f,
                          "concurrent source output initialized");
            }
        });
        while (!entered)
            std::this_thread::yield();
        for (unsigned int i = 0; i < 1000; ++i)
            device.setSource(i % 3 == 0 ? nullptr : (i % 3 == 1 ? first : second));
        // Fake stop joins the callback thread. Deadlocks if close holds the
        // source lock while waiting; races if it resets/uninitializes first.
        device.close();
    }
    {
        MiniAudioAudioDevice device("test", "fake", 0, false);
        check(device.openInput(), "open concurrent capture");
        callbackThread = std::thread([&] {
            for (unsigned int i = 0; i < 2000; ++i)
                record(0.75f, 961);
        });
        for (unsigned int i = 0; i < 1000; ++i)
            device.setSink(i % 2 == 0 ? nullptr : sink);
        // The destructor must also stop/join before releasing its members.
    }
    check(!callbackThread.joinable(), "destructor joined active callbacks");
}

void teardownWaitsForCallback()
{
    for (bool capture : {false, true}) {
        auto device = std::make_unique<MiniAudioAudioDevice>("test", "fake", 0, true);
        auto source = std::make_shared<BlockingSource>();
        auto sink = std::make_shared<BlockingSink>();
        CallbackBarrier& barrier = capture ? static_cast<CallbackBarrier&>(*sink)
                                           : static_cast<CallbackBarrier&>(*source);
        // Set the blocking endpoint after start: a real backend may invoke its
        // playback callback synchronously from start, as our fake also does.
        check(capture ? device->openInput() : device->openOutput(), "open barrier test");
        device->setSource(source);
        device->setSink(sink);
        callbackThread = std::thread([&] {
            if (capture)
                record(0.75f, frameSizeSamples);
            else
                render(0.5f, 1);
        });
        while (!barrier.entered)
            std::this_thread::yield();
        stopEntered = false;
        std::atomic<bool> closed{false};
        std::thread closer([&] {
            if (capture)
                device.reset(); // destructor follows the same drain protocol
            else
                device->close();
            closed = true;
        });
        while (!stopEntered)
            std::this_thread::yield();
        check(!closed, "teardown waits for in-flight callback");
        barrier.released = true;
        closer.join();
        check(closed && !callbackThread.joinable(), "teardown drains callback before return");
        check(activeDevice == nullptr && contexts.empty(), "drained teardown frees backend");
    }
}
}

// No real miniaudio code is linked into this executable.
ma_context_config ma_context_config_init() { return {}; }
ma_result ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context* context)
{
    if (failContext)
        return MA_ERROR;
    check(contexts.insert(context).second, "context initialized once");
    ++contextInits;
    return MA_SUCCESS;
}
ma_result ma_context_uninit(ma_context* context)
{
    check(!activeDevice || activeDevice->pContext != context, "device destroyed before context");
    check(contexts.erase(context) == 1, "only initialized contexts are uninitialized once");
    return MA_SUCCESS;
}
ma_log* ma_context_get_log(ma_context*) { return nullptr; }
ma_log_callback ma_log_callback_init(ma_log_callback_proc proc, void* data) { return {proc, data}; }
ma_result ma_log_register_callback(ma_log*, ma_log_callback) { return MA_SUCCESS; }
const char* ma_get_backend_name(ma_backend) { return "fake"; }
const char* ma_result_description(ma_result) { return "fake result"; }
const char* ma_log_level_to_string(ma_uint32) { return "fake level"; }
const char* ma_get_format_name(ma_format) { return "fake format"; }
ma_result ma_context_get_devices(ma_context*, ma_device_info** output, ma_uint32* outputs,
                                ma_device_info** input, ma_uint32* inputs)
{
    static ma_device_info info{};
    std::strcpy(info.name, "fake");
    if (output) { *output = &info; *outputs = 1; }
    if (input) { *input = &info; *inputs = 1; }
    return MA_SUCCESS;
}
ma_result ma_context_get_device_info(ma_context*, ma_device_type, const ma_device_id*, ma_device_info* info)
{
    *info = {};
    return MA_SUCCESS;
}
ma_device_config ma_device_config_init(ma_device_type type)
{
    ma_device_config config{};
    config.deviceType = type;
    return config;
}
ma_result ma_device_init(ma_context* context, const ma_device_config* config, ma_device* device)
{
    check(contexts.count(context) == 1 && activeDevice == nullptr, "init uses live context");
    if (failInit)
        return MA_ERROR;
    device->pContext = context;
    device->type = config->deviceType;
    device->pUserData = config->pUserData;
    device->playback.channels = config->playback.channels;
    device->capture.channels = config->capture.channels;
    callback = config->dataCallback;
    activeDevice = device;
    stopped = true;
    ++deviceInits;
    return MA_SUCCESS;
}
ma_result ma_device_start(ma_device* device)
{
    check(device == activeDevice, "start initialized device");
    ++starts;
    stopped = false;
    // Backends can call into the device before ma_device_start returns.
    if (device->type == ma_device_type_playback) {
        std::array<float, 2> output{};
        callback(device, output.data(), nullptr, 1);
    }
    return failStart ? MA_ERROR : MA_SUCCESS;
}
ma_result ma_device_stop(ma_device* device)
{
    check(device == activeDevice, "stop initialized device");
    stopEntered = true;
    if (callbackThread.joinable())
        callbackThread.join();
    stopped = true;
    ++stops;
    return MA_SUCCESS;
}
void ma_device_uninit(ma_device* device)
{
    check(device == activeDevice && stopped, "stop precedes uninit exactly once");
    check(!callbackThread.joinable(), "callbacks finished before uninit");
    activeDevice = nullptr;
    callback = nullptr;
    ++deviceUninits;
}
namespace afv_native {
void __Log(const char*, int, const char*, const char*, ...) {}
}

int main()
{
    idleAndFailures();
    restartAndEndpointTails();
    concurrentEndpointsAndClose();
    teardownWaitsForCallback();
    check(contexts.empty() && activeDevice == nullptr, "all resources released");
    check(deviceInits == deviceUninits && stops == deviceUninits, "balanced stop/uninit");
    std::puts("device lifecycle tests passed (fake backend only)");
}
