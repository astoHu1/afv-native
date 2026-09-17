#include "afv-native/audio/SimpleCompressorEffect.h"
#include <array>

using namespace afv_native::audio;

SimpleCompressorEffect::SimpleCompressorEffect()
{
    sf_defaultcomp(&m_simpleCompressor, sampleRateHz);
}

SimpleCompressorEffect::~SimpleCompressorEffect()
{

}

void SimpleCompressorEffect::transformFrame(SampleType *bufferOut, const SampleType bufferIn[])
{
    std::array<sf_sample_st, frameSizeSamples> output{};
    std::array<sf_sample_st, frameSizeSamples> input{};

    for(int i = 0; i < frameSizeSamples; i++)
    {
        input[i].L = bufferIn[i];
    }

    sf_compressor_process(&m_simpleCompressor, frameSizeSamples, input.data(), output.data());

    for(int i = 0; i < frameSizeSamples; i++)
    {
        bufferOut[i] = static_cast<SampleType>(output[i].L);
    }

}
