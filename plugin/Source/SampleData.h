#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace ss
{

/** A loaded sample: immutable once built, shared by reference count. */
struct SampleData : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<SampleData>;

    static constexpr int kOverview = 4096;
    static constexpr double kMaxSeconds = 30.0 * 60.0;

    juce::File file;
    juce::String name, formatName;
    double sampleRate = 44100;
    int numChannels = 1;
    int length = 0;
    juce::AudioBuffer<float> fwd, rev;            // rev = time-reversed copy (for REVERSE)
    std::vector<float> ovMin, ovMax;              // waveform overview (kOverview columns)
    float peak = 0;

    double seconds() const noexcept { return length / sampleRate; }

    static Ptr load (const juce::File& f, juce::AudioFormatManager& fm, juce::String& error)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
        if (reader == nullptr) { error = "unsupported or unreadable file"; return nullptr; }
        if (reader->lengthInSamples <= 0) { error = "empty file"; return nullptr; }
        if ((double) reader->lengthInSamples / reader->sampleRate > kMaxSeconds) { error = "file longer than 30 minutes"; return nullptr; }

        Ptr d = new SampleData();
        d->file = f;
        d->name = f.getFileName();
        d->formatName = reader->getFormatName();
        d->sampleRate = reader->sampleRate;
        d->numChannels = juce::jlimit (1, 2, (int) reader->numChannels);
        d->length = (int) reader->lengthInSamples;
        d->fwd.setSize (d->numChannels, d->length);
        reader->read (&d->fwd, 0, d->length, 0, true, d->numChannels > 1);

        d->rev.makeCopyOf (d->fwd);
        for (int c = 0; c < d->numChannels; ++c)
        {
            auto* p = d->rev.getWritePointer (c);
            std::reverse (p, p + d->length);
        }

        d->ovMin.assign (kOverview, 0.0f);
        d->ovMax.assign (kOverview, 0.0f);
        for (int x = 0; x < kOverview; ++x)
        {
            const int s0 = (int) ((int64_t) x * d->length / kOverview);
            const int s1 = juce::jmax (s0 + 1, (int) ((int64_t) (x + 1) * d->length / kOverview));
            float lo = 0, hi = 0;
            for (int c = 0; c < d->numChannels; ++c)
            {
                auto* p = d->fwd.getReadPointer (c);
                for (int i = s0; i < juce::jmin (s1, d->length); ++i)
                {
                    lo = juce::jmin (lo, p[i]);
                    hi = juce::jmax (hi, p[i]);
                }
            }
            d->ovMin[(size_t) x] = lo;
            d->ovMax[(size_t) x] = hi;
            d->peak = juce::jmax (d->peak, hi, -lo);
        }
        return d;
    }
};

} // namespace ss
