#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "StretchEngine.h"

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

/**
 * The region [a, b) of `d` as a SourceView, slid by `windowPct` % of the room
 * it has to travel (100 % = from where it was drawn all the way to the far end),
 * reflecting off both ends of the sample. b - a < 16 means the whole sample.
 * Shared by live playback and export, so both move the window identically.
 */
inline SourceView regionView (const SampleData& d, int a, int b, float windowPct) noexcept
{
    a = juce::jlimit (0, d.length, a);
    b = juce::jlimit (0, d.length, b);
    if (b - a < 16) { a = 0; b = d.length; }
    const int len = b - a, room = d.length - len;
    if (windowPct != 0.0f && room > 0)
    {
        const double period = 2.0 * room;
        double x = std::fmod (a + windowPct / 100.0 * room, period);
        if (x < 0) x += period;
        if (x > room) x = period - x;
        a = juce::jlimit (0, room, (int) x);
        b = a + len;
    }
    SourceView v;
    v.numChannels = d.numChannels;
    v.len = len;
    v.sampleRate = d.sampleRate;
    for (int c = 0; c < d.numChannels; ++c)
    {
        v.fwd[c] = d.fwd.getReadPointer (c) + a;
        v.rev[c] = d.rev.getReadPointer (c) + (d.length - b);
    }
    return v;
}

} // namespace ss
