/*
 * Recorder — captures exactly what Slow Sippin outputs, live, to a WAV file.
 *
 * The audio thread only pushes samples into a lock-free FIFO
 * (juce::AudioFormatWriter::ThreadedWriter); a background thread writes them
 * to disk, so recording never causes dropouts. The file is written directly
 * to its final place as it records, so a take is never lost (even if the
 * plugin is closed while recording, the destructor finishes the file).
 */
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

namespace ss
{

class Recorder
{
public:
    Recorder() : diskThread ("Slow Sippin recorder") {}
    ~Recorder() { stop(); }

    /** Message thread. bits: 16 | 24 | 32 (float). */
    bool start (const juce::File& file, double sampleRate, int bits, juce::String& error)
    {
        stop();
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        if (! static_cast<juce::FileOutputStream*> (out.get())->openedOk()) { error = "cannot write " + file.getFullPathName(); return false; }

        auto opts = juce::AudioFormatWriterOptions()
                        .withSampleRate (sampleRate)
                        .withNumChannels (2)
                        .withBitsPerSample (bits)
                        .withSampleFormat (bits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                      : juce::AudioFormatWriterOptions::SampleFormat::integral);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (out, opts);
        if (writer == nullptr) { error = "cannot create a WAV writer at " + juce::String (sampleRate) + " Hz"; return false; }

        diskThread.startThread (juce::Thread::Priority::normal);
        threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), diskThread, 1 << 18);
        currentFile = file;
        rate = sampleRate;
        samples.store (0);
        dropped.store (0);
        peak.store (0);
        {
            const juce::ScopedLock sl (lock);
            active = threaded.get();
        }
        return true;
    }

    /** Message thread. Finishes the file; returns it (or an empty File if nothing was recording). */
    juce::File stop()
    {
        {
            const juce::ScopedLock sl (lock);
            active = nullptr;
        }
        if (threaded == nullptr) return {};
        threaded.reset();                  // flushes the FIFO and closes the file
        auto f = currentFile;
        currentFile = juce::File();
        return f;
    }

    /** Audio thread: add one block of the output. */
    void process (const float* L, const float* R, int n) noexcept
    {
        const juce::ScopedTryLock sl (lock);
        if (! sl.isLocked() || active == nullptr) return;
        const float* ch[] = { L, R };
        if (! active->write (ch, n)) dropped.fetch_add (n);
        samples.fetch_add (n);
        float p = 0;
        for (int i = 0; i < n; ++i) p = juce::jmax (p, std::abs (L[i]), std::abs (R[i]));
        peak.store (juce::jmax (p, peak.load() * 0.85f));
    }

    bool isRecording() const noexcept { return threaded != nullptr; }
    double seconds() const noexcept { return rate > 0 ? (double) samples.load() / rate : 0.0; }
    float level() const noexcept { return peak.load(); }
    int64_t droppedSamples() const noexcept { return dropped.load(); }
    juce::File file() const { return currentFile; }

private:
    juce::TimeSliceThread diskThread;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threaded;
    juce::CriticalSection lock;
    juce::AudioFormatWriter::ThreadedWriter* active = nullptr;
    juce::File currentFile;
    double rate = 48000;
    std::atomic<int64_t> samples { 0 }, dropped { 0 };
    std::atomic<float> peak { 0 };
};

} // namespace ss
