// Standalone, hardware-free test:
// c++ -std=c++17 -Iclient/afv-native/include \
//     client/afv-native/tests/device_frame_adapter_tests.cpp -o /tmp/device_frames
#include "../src/audio/DeviceFrameAdapter.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
bool countAllocations = false;
std::size_t allocations = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations)
        ++allocations;
    if (void* result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

using namespace afv_native::audio;

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

float sample(std::size_t frame, unsigned int channel)
{
    const float value = static_cast<float>(frame % 511 + 1) / 512.0f;
    return channel == 0 ? value : -value * 0.5f;
}

struct SequenceSource : ISampleSource {
    explicit SequenceSource(unsigned int channelCount) : channels(channelCount) {}
    SourceStatus getAudioFrame(float* output) override
    {
        ++calls;
        for (std::size_t i = 0; i < frameSizeSamples; ++i)
            for (unsigned int c = 0; c < channels; ++c)
                output[i * channels + c] = sample(nextFrame + i, c);
        nextFrame += frameSizeSamples;
        return SourceStatus::OK;
    }
    unsigned int channels;
    std::size_t nextFrame = 0;
    std::size_t calls = 0;
};

struct CheckingSink : ISampleSink {
    explicit CheckingSink(unsigned int channelCount) : channels(channelCount) {}
    void putAudioFrame(const float* input) override
    {
        for (std::size_t i = 0; i < frameSizeSamples; ++i) {
            const float expected = channels == 1 ? sample(nextFrame + i, 0)
                : sample(nextFrame + i, 0) * 0.5f + sample(nextFrame + i, 1) * 0.5f;
            check(input[i] == expected, "capture continuity/downmix");
        }
        nextFrame += frameSizeSamples;
        ++calls;
    }
    unsigned int channels;
    std::size_t nextFrame = 0;
    std::size_t calls = 0;
};

struct ConstantSource : ISampleSource {
    SourceStatus getAudioFrame(float* output) override
    {
        ++calls;
        std::fill_n(output, written, value);
        return status;
    }
    std::size_t written = frameSizeSamples * 2;
    float value = 0.25f;
    SourceStatus status = SourceStatus::OK;
    unsigned int calls = 0;
};

struct ConstantSink : ISampleSink {
    void putAudioFrame(const float* input) override
    {
        ++calls;
        for (std::size_t i = 0; i < frameSizeSamples; ++i)
            check(input[i] == expected, "capture tail reset");
    }
    float expected = 0.75f;
    unsigned int calls = 0;
};

void playbackContinuity(unsigned int channels)
{
    DeviceFrameAdapter adapter;
    SequenceSource source(channels);
    std::size_t position = 0;
    // Deliberately split source frames across callbacks and cross several
    // source frames in one callback. Vector size gives ASan exact boundaries.
    for (std::size_t frames : {0, 1, 127, 959, 960, 961, 1921, 4097, 3, 77}) {
        std::vector<float> output(frames * channels + 2, 99.0f);
        adapter.playback(output.data() + 1, frames, channels, &source);
        check(output.front() == 99.0f && output.back() == 99.0f, "playback guards");
        for (std::size_t i = 0; i < frames; ++i)
            for (unsigned int c = 0; c < channels; ++c)
                check(output[1 + i * channels + c] == sample(position + i, c),
                      "playback continuity/interleaving");
        position += frames;
        check(source.calls == (position + frameSizeSamples - 1) / frameSizeSamples,
              "playback fetches only needed source frames");
    }
    adapter.playback(nullptr, 100, channels, &source);
}

void captureContinuity(unsigned int channels)
{
    DeviceFrameAdapter adapter;
    CheckingSink sink(channels);
    std::size_t position = 0;
    for (std::size_t frames : {0, 1, 127, 959, 960, 961, 1921, 4097, 3, 77}) {
        std::vector<float> input(frames * channels);
        for (std::size_t i = 0; i < frames; ++i)
            for (unsigned int c = 0; c < channels; ++c)
                input[i * channels + c] = sample(position + i, c);
        adapter.capture(input.data(), frames, channels, &sink);
        position += frames;
        check(sink.calls == position / frameSizeSamples, "capture delivers only whole frames");
    }
}

void silenceAndFailures(unsigned int channels)
{
    DeviceFrameAdapter adapter;
    std::array<float, (frameSizeSamples * 2 + 13) * 2> output;
    const std::size_t frames = frameSizeSamples * 2 + 13;
    auto silent = [&] {
        for (std::size_t i = 0; i < frames * channels; ++i)
            check(output[i] == 0.0f, "all output samples must be silent");
    };
    output.fill(99.0f);
    adapter.playback(output.data(), frames, channels, nullptr);
    silent();

    for (auto status : {SourceStatus::Error, SourceStatus::Closed}) {
        ConstantSource source;
        source.status = status;
        source.written = 17; // A failing source may have already dirtied output.
        adapter.resetPlayback();
        output.fill(99.0f);
        adapter.playback(output.data(), frames, channels, &source);
        silent();
        adapter.playback(output.data(), frames, channels, &source);
        silent();
        check(source.calls == 1, "failed source not polled again until reset");
        adapter.resetPlayback();
        source.status = SourceStatus::OK;
        source.written = frameSizeSamples * channels;
        adapter.playback(output.data(), 1, channels, &source);
        check(output[0] == source.value, "source reset clears failure latch");
    }

    ConstantSource partial;
    adapter.resetPlayback();
    adapter.playback(output.data(), frameSizeSamples, channels, &partial);
    partial.written = 7;
    adapter.playback(output.data(), frameSizeSamples * 2, channels, &partial);
    for (std::size_t i = 0; i < frameSizeSamples * 2 * channels; ++i)
        check(output[i] == (i % (frameSizeSamples * channels) < 7 ? partial.value : 0.0f),
              "unwritten successful source samples are zero");

    // A successful buffered tail can precede a failed frame in one callback.
    ConstantSource source;
    adapter.resetPlayback();
    adapter.playback(output.data(), 13, channels, &source);
    source.status = SourceStatus::Error;
    adapter.playback(output.data(), frameSizeSamples, channels, &source);
    for (std::size_t i = 0; i < frameSizeSamples * channels; ++i)
        check(output[i] == (i < (frameSizeSamples - 13) * channels ? source.value : 0.0f),
              "failure silences entire failed frame after buffered tail");
}

void resetTails()
{
    DeviceFrameAdapter adapter;
    ConstantSource source;
    std::array<float, frameSizeSamples * 2> buffer{};
    adapter.playback(buffer.data(), 13, 2, &source);
    adapter.resetPlayback(); // close/reopen, or a source replacement
    source.value = 0.75f;
    adapter.playback(buffer.data(), 17, 2, &source);
    check(buffer[0] == 0.75f && buffer[33] == 0.75f, "restart drops playback tail");
    adapter.playback(buffer.data(), 17, 2, nullptr);
    check(buffer[0] == 0.0f && buffer[33] == 0.0f, "detach silences buffered tail");
    source.value = 0.5f;
    adapter.playback(buffer.data(), 17, 2, &source);
    check(buffer[0] == 0.5f, "reattach drops playback tail");
    source.value = 0.25f;
    adapter.playback(buffer.data(), 17, 1, &source);
    check(buffer[0] == 0.25f, "channel change drops playback tail");

    ConstantSink sink;
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        adapter.resetCapture();
        buffer.fill(0.25f);
        adapter.capture(buffer.data(), 117, 1, &sink);
        if (scenario == 0)
            adapter.resetCapture(); // close/reopen, or sink replacement
        else if (scenario == 1)
            adapter.capture(nullptr, 1, 1, &sink);
        else if (scenario == 2)
            adapter.capture(buffer.data(), 1, 1, nullptr);
        // scenario 3 switches channels, which must also discard old samples.
        buffer.fill(sink.expected);
        const auto calls = sink.calls;
        const auto channels = scenario == 3 ? 2u : 1u;
        adapter.capture(buffer.data(), frameSizeSamples - 1, channels, &sink);
        check(sink.calls == calls, "partial capture must not leak before completion");
        adapter.capture(buffer.data(), 1, channels, &sink);
        check(sink.calls == calls + 1, "capture resumes after reset");
    }
}

void noCallbackAllocations()
{
    DeviceFrameAdapter adapter;
    ConstantSource source;
    ConstantSink sink;
    std::array<float, 4097 * 2> output{};
    std::array<float, 4097 * 2> input;
    input.fill(sink.expected);
    allocations = 0;
    countAllocations = true;
    for (unsigned int channels : {1, 2}) {
        adapter.resetPlayback();
        adapter.resetCapture();
        for (unsigned int i = 0; i < 100; ++i) {
            adapter.playback(output.data(), 4097, channels, &source);
            adapter.capture(input.data(), 4097, channels, &sink);
        }
    }
    countAllocations = false;
    check(allocations == 0, "frame adapter allocated during callbacks");
}
}

int main()
{
    for (unsigned int channels : {1, 2}) {
        playbackContinuity(channels);
        captureContinuity(channels);
        silenceAndFailures(channels);
    }
    resetTails();
    noCallbackAllocations();
    std::puts("device frame adapter tests passed");
}
