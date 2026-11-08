#include "VoiceEngine.h"
#include <thread>

namespace ss
{

// ---------------------------------------------------------------- worker --

class VoiceEngine::Worker : public juce::Thread
{
public:
    Worker (VoiceEngine& e, int i, int n)
        : juce::Thread ("Slow Sippin render " + juce::String (i)), engine (e), index (i), count (n)
    {
        tL.assign (kChunk, 0.0f);
        tR.assign (kChunk, 0.0f);
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            const int work = engine.workerPass (index, count, tL.data(), tR.data());
            if (work == 0) wait (1);
        }
    }

private:
    VoiceEngine& engine;
    int index, count;
    std::vector<float> tL, tR;
};

VoiceEngine::VoiceEngine() = default;
VoiceEngine::~VoiceEngine() { shutdown(); }

void VoiceEngine::prepare (double sampleRate, int maxBlock)
{
    shutdown();
    rate = sampleRate;
    target = juce::jlimit (6144, kFifoSize - 2 * kChunk, 4 * juce::jmax (256, maxBlock));
    for (auto& v : voices)
    {
        v.stream.prepare (sampleRate);
        v.fifo.reset();
        v.bufL.assign (kFifoSize, 0.0f);
        v.bufR.assign (kFifoSize, 0.0f);
        v.env.setSampleRate (sampleRate);
        v.active = v.releasing = v.transport = v.sustained = false;
        v.gen.store (0);
        v.ack.store (0);
        v.wgen = 0;
        v.streaming = false;
        v.data = nullptr;
        v.sounding.store (false);
    }
    tmpL.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);
    tmpR.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);
    transportOn.store (false);

    const int hw = (int) std::thread::hardware_concurrency();
    const int n = juce::jlimit (1, 4, hw > 1 ? hw - 1 : 1);
    for (int i = 0; i < n; ++i)
    {
        workers.push_back (std::make_unique<Worker> (*this, i, n));
        workers.back()->startThread (juce::Thread::Priority::high);
    }
}

void VoiceEngine::shutdown()
{
    for (auto& w : workers) w->signalThreadShouldExit();
    for (auto& w : workers) w->stopThread (2000);
    workers.clear();
    for (auto& v : voices) v.data = nullptr;
}

// ------------------------------------------------------------ commands --

void VoiceEngine::command (Voice& v, int c, double frac, uint32_t seed) noexcept
{
    v.cmd.store (c, std::memory_order_relaxed);
    v.cmdFrac.store (frac, std::memory_order_relaxed);
    v.cmdSeed.store (seed, std::memory_order_relaxed);
    v.gen.fetch_add (1, std::memory_order_release);
}

void VoiceEngine::setEnvelope (float attackMs, float releaseMs) noexcept
{
    attack = attackMs;
    release = releaseMs;
    for (auto& v : voices)
    {
        if (! v.active || v.quickRelease > 0) continue;
        juce::ADSR::Parameters p { attack / 1000.0f, 0.0f, 1.0f, juce::jmax (0.005f, release / 1000.0f) };
        v.env.setParameters (p);
    }
}

void VoiceEngine::noteOn (int note, float velocity, bool transport, double startFrac, uint32_t seed, int64_t startTime) noexcept
{
    // free voice (fully idle) > releasing oldest > oldest
    Voice* pick = nullptr;
    for (auto& v : voices)
        if (! v.active && v.ack.load (std::memory_order_acquire) == v.gen.load (std::memory_order_relaxed)) { pick = &v; break; }
    if (pick == nullptr)
        for (auto& v : voices)
            if (! v.active) { pick = &v; break; }
    if (pick == nullptr)
        for (auto& v : voices)
            if (v.releasing && (pick == nullptr || v.age < pick->age)) pick = &v;
    if (pick == nullptr)
        for (auto& v : voices)
            if (! v.transport && (pick == nullptr || v.age < pick->age)) pick = &v;
    if (pick == nullptr) pick = &voices[0];

    Voice& v = *pick;
    v.active = true;
    v.releasing = false;
    v.sustained = false;
    v.transport = transport;
    v.note = note;
    v.velocity = velocity;
    v.quickRelease = 0;
    v.age = ++ageCounter;
    v.ended.store (false);
    v.noteSemis.store ((keyTrack && ! transport ? (float) (note - 60) : 0.0f) + bend);
    juce::ADSR::Parameters p { attack / 1000.0f, 0.0f, 1.0f, juce::jmax (0.005f, release / 1000.0f) };
    v.env.setParameters (p);
    v.env.reset();
    v.env.noteOn();
    v.cmdTime.store (startTime, std::memory_order_relaxed);
    command (v, 1, startFrac, seed);
    if (transport) transportOn.store (true);
}

void VoiceEngine::noteOff (int note) noexcept
{
    for (auto& v : voices)
    {
        if (! v.active || v.transport || v.note != note || v.releasing) continue;
        if (sustainDown) { v.sustained = true; continue; }
        v.releasing = true;
        v.env.noteOff();
    }
}

void VoiceEngine::setSustain (bool down) noexcept
{
    sustainDown = down;
    if (down) return;
    for (auto& v : voices)
        if (v.active && v.sustained && ! v.releasing)
        {
            v.sustained = false;
            v.releasing = true;
            v.env.noteOff();
        }
}

void VoiceEngine::setBend (float semis) noexcept
{
    bend = semis;
    for (auto& v : voices)
        if (v.active)
            v.noteSemis.store ((keyTrack && ! v.transport ? (float) (v.note - 60) : 0.0f) + bend);
}

void VoiceEngine::stopTransport() noexcept
{
    for (auto& v : voices)
    {
        if (! v.active || ! v.transport) continue;
        v.quickRelease = 0.06f;
        v.env.setParameters ({ attack / 1000.0f, 0.0f, 1.0f, v.quickRelease });
        v.env.noteOff();
        v.releasing = true;
    }
    transportOn.store (false);
}

void VoiceEngine::jumpTransport (double frac) noexcept
{
    for (auto& v : voices)
        if (v.active && v.transport && ! v.releasing)
            command (v, 2, frac, 0);
}

void VoiceEngine::finishVoice (Voice& v) noexcept
{
    v.active = false;
    v.releasing = false;
    v.sounding.store (false);
    if (v.transport) transportOn.store (false);
    v.transport = false;
    command (v, 3, 0, 0);
}

void VoiceEngine::killAll() noexcept
{
    for (auto& v : voices)
        if (v.active) finishVoice (v);
    transportOn.store (false);
}

// -------------------------------------------------------------- render --

void VoiceEngine::render (float* L, float* R, int n, bool offline) noexcept
{
    for (auto& v : voices)
    {
        const uint32_t g = v.gen.load (std::memory_order_relaxed);
        if (v.ack.load (std::memory_order_acquire) != g)
        {
            // stale samples from before the last command: discard
            const int ready = v.fifo.getNumReady();
            if (ready > 0) { auto h = v.fifo.read (ready); juce::ignoreUnused (h); }
            if (! offline || ! v.active) continue;
            const auto t0 = juce::Time::getMillisecondCounter();
            while (v.ack.load (std::memory_order_acquire) != g && juce::Time::getMillisecondCounter() - t0 < 3000)
            {
                const int r = v.fifo.getNumReady();
                if (r > 0) { auto h = v.fifo.read (r); juce::ignoreUnused (h); }
                std::this_thread::yield();
            }
            if (v.ack.load (std::memory_order_acquire) != g) continue;
        }
        if (! v.active) continue;

        int ready = v.fifo.getNumReady();
        if (offline && ready < n && ! v.ended.load())
        {
            const auto t0 = juce::Time::getMillisecondCounter();
            while ((ready = v.fifo.getNumReady()) < n && ! v.ended.load() && juce::Time::getMillisecondCounter() - t0 < 5000)
                std::this_thread::yield();
        }
        const int k = juce::jmin (n, ready);
        if (k < n && ! v.ended.load() && v.sounding.load()) underrunCount.fetch_add (1);

        if (k > 0)
        {
            const auto h = v.fifo.read (k);
            int o = 0;
            auto take = [&] (int start, int size)
            {
                for (int i = 0; i < size; ++i, ++o)
                {
                    const float e = v.env.getNextSample() * v.velocity;
                    L[o] += v.bufL[(size_t) (start + i)] * e;
                    R[o] += v.bufR[(size_t) (start + i)] * e;
                }
            };
            take (h.startIndex1, h.blockSize1);
            take (h.startIndex2, h.blockSize2);
            v.sounding.store (true);
        }

        const bool envDone = v.releasing && ! v.env.isActive();
        const bool streamDone = v.ended.load() && v.fifo.getNumReady() == 0;
        if (envDone || streamDone) finishVoice (v);
    }
}

int VoiceEngine::workerPass (int index, int count, float* tL, float* tR)
{
    int work = 0;
    for (int i = index; i < kMaxVoices; i += count)
    {
        Voice& v = voices[i];
        const uint32_t g = v.gen.load (std::memory_order_acquire);
        if (g != v.wgen)
        {
            if (v.fifo.getNumReady() > 0) { ++work; continue; }   // wait until the audio thread drained it
            const int c = v.cmd.load (std::memory_order_relaxed);
            if (c == 1)
            {
                v.data = currentSample ? currentSample() : nullptr;
                v.t0 = v.cmdTime.load (std::memory_order_relaxed);
                v.rendered = 0;
                if (v.data != nullptr)
                {
                    const auto view = shared->view (*v.data);
                    v.stream.start (v.cmdFrac.load(), v.cmdSeed.load(), shared->reverse.load(), view.len);
                    v.streaming = view.len > 0;
                }
                else v.streaming = false;
                v.ended.store (! v.streaming);
            }
            else if (c == 2 && v.data != nullptr)
            {
                v.stream.jumpTo (v.cmdFrac.load(), shared->view (*v.data).len);
                v.streaming = true;
                v.ended.store (false);
            }
            else
            {
                v.streaming = false;
                v.data = nullptr;          // released here, off the audio thread
            }
            v.wgen = g;
            v.ack.store (g, std::memory_order_release);
        }
        if (! v.streaming || v.data == nullptr) continue;
        if (v.fifo.getNumReady() >= target || v.fifo.getFreeSpace() < kChunk) continue;

        std::fill (tL, tL + kChunk, 0.0f);
        std::fill (tR, tR + kChunk, 0.0f);
        float wnd = 0;
        const auto st = shared->snapshotAt (v.noteSemis.load (std::memory_order_relaxed), v.t0 + v.rendered, wnd);
        const auto view = shared->view (*v.data, wnd);
        const int got = v.stream.render (view, st, tL, tR, kChunk, 1.0f);
        v.rendered += got;
        v.srcFrac.store ((float) v.stream.sourceFrac());
        v.posFrac.store ((float) v.stream.positionFrac());

        if (v.gen.load (std::memory_order_acquire) != g) continue;    // a new command arrived meanwhile
        const auto h = v.fifo.write (got);
        std::copy (tL, tL + h.blockSize1, v.bufL.begin() + h.startIndex1);
        std::copy (tR, tR + h.blockSize1, v.bufR.begin() + h.startIndex1);
        std::copy (tL + h.blockSize1, tL + h.blockSize1 + h.blockSize2, v.bufL.begin() + h.startIndex2);
        std::copy (tR + h.blockSize1, tR + h.blockSize1 + h.blockSize2, v.bufR.begin() + h.startIndex2);
        if (got < kChunk)
        {
            v.streaming = false;
            v.ended.store (true);
        }
        ++work;
    }
    return work;
}

// ------------------------------------------------------------------- UI --

float VoiceEngine::transportFrac() const noexcept
{
    for (auto& v : voices)
        if (v.transport && v.sounding.load()) return v.posFrac.load();
    return -1.0f;
}

int VoiceEngine::positions (float* out, int maxCount) const noexcept
{
    int c = 0;
    for (auto& v : voices)
        if (v.sounding.load() && c < maxCount) out[c++] = v.srcFrac.load();
    return c;
}

} // namespace ss
