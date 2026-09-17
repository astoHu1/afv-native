#ifndef AFV_NATIVE_DEVICEFRAMEADAPTER_H
#define AFV_NATIVE_DEVICEFRAMEADAPTER_H

#include "afv-native/audio/ISampleSink.h"
#include "afv-native/audio/ISampleSource.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace afv_native {
namespace audio {

// Converts arbitrary device callback lengths to the fixed AFV frame size.
// Playback sources supply the requested mono or interleaved stereo format;
// capture sinks always receive mono. Storage is fixed and never allocated in a
// callback. The owner serializes each direction with its endpoint setters and
// resets it after callbacks have stopped, before restarting the device.
class DeviceFrameAdapter {
public:
    void resetPlayback()
    {
        mPlayback.fill(0.0f);
        mPlaybackOffset = frameSizeSamples;
        mPlaybackChannels = 0;
        mPlaybackFailed = false;
    }

    void resetCapture()
    {
        mCapture.fill(0.0f);
        mCaptureSize = 0;
        mCaptureChannels = 0;
    }

    void playback(SampleType* output, std::size_t frames, unsigned int channels,
                  ISampleSource* source)
    {
        if (!output || frames == 0)
            return;

        // Silence is the default, including a source that fails after writing
        // only part of a frame, or an OK source that leaves samples unwritten.
        std::fill_n(output, frames * channels, 0.0f);
        if (!source || channels < 1 || channels > 2) {
            resetPlayback();
            return;
        }
        if (mPlaybackChannels != channels) {
            resetPlayback();
            mPlaybackChannels = channels;
        }

        while (frames > 0 && !mPlaybackFailed) {
            if (mPlaybackOffset == frameSizeSamples) {
                mPlayback.fill(0.0f);
                if (source->getAudioFrame(mPlayback.data()) != SourceStatus::OK) {
                    // Keep the shared source owned by the device: destroying it
                    // here could free memory or run arbitrary callback code.
                    // A new setSource() explicitly enables polling again.
                    mPlaybackFailed = true;
                    return;
                }
                mPlaybackOffset = 0;
            }
            const auto count = std::min(frames, frameSizeSamples - mPlaybackOffset);
            std::copy_n(mPlayback.data() + mPlaybackOffset * channels,
                        count * channels, output);
            output += count * channels;
            frames -= count;
            mPlaybackOffset += count;
        }
    }

    void capture(const SampleType* input, std::size_t frames, unsigned int channels,
                 ISampleSink* sink)
    {
        if (frames == 0)
            return;
        if (!input || !sink || channels < 1 || channels > 2) {
            // A missing buffer/endpoint breaks continuity; do not splice an
            // earlier partial frame onto a later microphone stream.
            resetCapture();
            return;
        }
        if (mCaptureChannels != channels) {
            resetCapture();
            mCaptureChannels = channels;
        }

        while (frames > 0) {
            const auto count = std::min(frames, frameSizeSamples - mCaptureSize);
            if (channels == 1) {
                std::copy_n(input, count, mCapture.data() + mCaptureSize);
            } else {
                for (std::size_t i = 0; i < count; ++i)
                    mCapture[mCaptureSize + i] = input[2 * i] * 0.5f + input[2 * i + 1] * 0.5f;
            }
            input += count * channels;
            frames -= count;
            mCaptureSize += count;
            if (mCaptureSize == frameSizeSamples) {
                sink->putAudioFrame(mCapture.data());
                mCaptureSize = 0;
            }
        }
    }

private:
    std::array<SampleType, frameSizeSamples * 2> mPlayback{};
    std::size_t mPlaybackOffset = frameSizeSamples;
    unsigned int mPlaybackChannels = 0;
    bool mPlaybackFailed = false;
    std::array<SampleType, frameSizeSamples> mCapture{};
    std::size_t mCaptureSize = 0;
    unsigned int mCaptureChannels = 0;
};

} // namespace audio
} // namespace afv_native

#endif
