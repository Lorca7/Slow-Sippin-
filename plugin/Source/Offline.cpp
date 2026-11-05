#include "Offline.h"

namespace ss
{

static constexpr uint32_t kIrSeed = 90210;

SourceView makeView (const SampleData& d, int a, int b) noexcept
{
    a = juce::jlimit (0, d.length, a);
    b = juce::jlimit (0, d.length, b);
    if (b - a < 16) { a = 0; b = d.length; }
    SourceView v;
    v.numChannels = d.numChannels;
    v.len = b - a;
    v.sampleRate = d.sampleRate;
    for (int c = 0; c < d.numChannels; ++c)
    {
        v.fwd[c] = d.fwd.getReadPointer (c) + a;
        v.rev[c] = d.rev.getReadPointer (c) + (d.length - b);
    }
    return v;
}

// --------------------------------------------------------------- writer --

static void put32 (juce::OutputStream& o, uint32_t v, bool le) { le ? o.writeInt ((int) v) : o.writeIntBigEndian ((int) v); }
static void put16 (juce::OutputStream& o, uint16_t v, bool le) { le ? o.writeShort ((short) v) : o.writeShortBigEndian ((short) v); }

static void putExt80 (juce::OutputStream& o, double v)
{
    const int e = (int) std::floor (std::log2 (v));
    const double m = v / std::pow (2.0, e);
    const uint32_t hi = (uint32_t) std::floor (m * 2147483648.0);
    const uint32_t lo = (uint32_t) std::floor ((m * 2147483648.0 - hi) * 4294967296.0);
    o.writeShortBigEndian ((short) (e + 16383));
    o.writeIntBigEndian ((int) hi);
    o.writeIntBigEndian ((int) lo);
}

bool writeAudioFile (const juce::File& f, int format, int bits, const std::vector<const float*>& chans,
                     size_t frames, double sampleRate, float gain, juce::String& error)
{
    const int ch = (int) chans.size();
    const int bps = bits / 8;
    const uint64_t dataLen = (uint64_t) frames * (uint64_t) ch * (uint64_t) bps;
    if (dataLen + 128 > 0xFFFFFFFFull) { error = "file would be larger than 4 GB"; return false; }

    f.deleteFile();
    juce::FileOutputStream o (f, 1 << 20);
    if (! o.openedOk()) { error = "cannot write " + f.getFullPathName(); return false; }

    const bool wav = format == 0;
    const bool isFloat = bits == 32;
    const int sr = (int) std::lround (sampleRate);
    if (wav)
    {
        o.write ("RIFF", 4); put32 (o, (uint32_t) (36 + dataLen), true); o.write ("WAVE", 4);
        o.write ("fmt ", 4); put32 (o, 16, true);
        put16 (o, isFloat ? 3 : 1, true);
        put16 (o, (uint16_t) ch, true);
        put32 (o, (uint32_t) sr, true);
        put32 (o, (uint32_t) (sr * ch * bps), true);
        put16 (o, (uint16_t) (ch * bps), true);
        put16 (o, (uint16_t) bits, true);
        o.write ("data", 4); put32 (o, (uint32_t) dataLen, true);
    }
    else
    {
        const uint32_t commLen = isFloat ? 18 + 4 + 14 : 18;
        const uint32_t fver = isFloat ? 12 : 0;
        const uint32_t headLen = 12 + fver + 8 + commLen + 16;
        o.write ("FORM", 4); put32 (o, (uint32_t) (headLen - 8 + dataLen), false); o.write (isFloat ? "AIFC" : "AIFF", 4);
        if (isFloat) { o.write ("FVER", 4); put32 (o, 4, false); put32 (o, 0xA2805140u, false); }
        o.write ("COMM", 4); put32 (o, commLen, false);
        put16 (o, (uint16_t) ch, false);
        put32 (o, (uint32_t) frames, false);
        put16 (o, (uint16_t) bits, false);
        putExt80 (o, sampleRate);
        if (isFloat)
        {
            o.write ("fl32", 4);
            o.writeByte (12); o.write ("32-bit float", 12); o.writeByte (0);
        }
        o.write ("SSND", 4); put32 (o, (uint32_t) (8 + dataLen), false); put32 (o, 0, false); put32 (o, 0, false);
    }

    Rng rng (0x1234567u);
    const size_t chunk = 1 << 16;
    juce::HeapBlock<uint8_t> buf (chunk * (size_t) ch * (size_t) bps);
    for (size_t f0 = 0; f0 < frames; f0 += chunk)
    {
        const size_t f1 = std::min (frames, f0 + chunk);
        uint8_t* p = buf.get();
        for (size_t i = f0; i < f1; ++i)
        {
            for (int c = 0; c < ch; ++c)
            {
                const double v = (double) chans[(size_t) c][i] * gain;
                if (isFloat)
                {
                    uint32_t u;
                    const float fv = (float) v;
                    std::memcpy (&u, &fv, 4);
                    if (wav) { p[0] = (uint8_t) u; p[1] = (uint8_t) (u >> 8); p[2] = (uint8_t) (u >> 16); p[3] = (uint8_t) (u >> 24); }
                    else     { p[3] = (uint8_t) u; p[2] = (uint8_t) (u >> 8); p[1] = (uint8_t) (u >> 16); p[0] = (uint8_t) (u >> 24); }
                }
                else if (bits == 16)
                {
                    const int q = juce::jlimit (-32768, 32767, (int) std::lround (v * 32767.0 + (rng.next() - rng.next())));
                    if (wav) { p[0] = (uint8_t) q; p[1] = (uint8_t) (q >> 8); }
                    else     { p[1] = (uint8_t) q; p[0] = (uint8_t) (q >> 8); }
                }
                else
                {
                    const int q = juce::jlimit (-8388608, 8388607, (int) std::lround (v * 8388607.0));
                    if (wav) { p[0] = (uint8_t) q; p[1] = (uint8_t) (q >> 8); p[2] = (uint8_t) (q >> 16); }
                    else     { p[2] = (uint8_t) q; p[1] = (uint8_t) (q >> 8); p[0] = (uint8_t) (q >> 16); }
                }
                p += bps;
            }
        }
        if (! o.write (buf.get(), (size_t) (p - buf.get()))) { error = "disk full or write error"; return false; }
    }
    o.flush();
    if (o.getStatus().failed()) { error = o.getStatus().getErrorMessage(); return false; }
    return true;
}

// ------------------------------------------------------------- analyzer --

void Analyzer::request (const OfflineSettings& s, double outputRate)
{
    {
        const juce::ScopedLock sl (lock);
        pending = s;
        pendingRate = outputRate;
        hasPending = true;
    }
    wake.signal();
}

void Analyzer::run()
{
    StretchStream stream;
    double preparedRate = 0;
    std::vector<float> L (4096), R (4096);
    while (! threadShouldExit())
    {
        wake.wait (500);
        OfflineSettings s;
        double rate;
        {
            const juce::ScopedLock sl (lock);
            if (! hasPending) continue;
            s = pending;
            rate = pendingRate;
            hasPending = false;
        }
        if (s.data == nullptr) { lastPeak.store (0); continue; }
        if (rate != preparedRate) { stream.prepare (rate); preparedRate = rate; }

        const auto view = makeView (*s.data, s.regionA, s.regionB);
        stream.start (0.0, 0xC0FFEEu, s.stream.reverse, view.len);
        const double outSecs = view.len / view.sampleRate * s.stream.stretch;
        const int64_t total = (int64_t) (juce::jmin (12.0, outSecs + 0.3) * rate);
        float peak = 0;
        bool interrupted = false;
        for (int64_t p = 0; p < total; p += 4096)
        {
            if (threadShouldExit()) return;
            { const juce::ScopedLock sl (lock); if (hasPending) { interrupted = true; break; } }
            std::fill (L.begin(), L.end(), 0.0f);
            std::fill (R.begin(), R.end(), 0.0f);
            const int got = stream.render (view, s.stream, L.data(), R.data(), 4096, 1.0f);
            for (int i = 0; i < got; ++i) peak = juce::jmax (peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]));
            if (got < 4096) break;
        }
        if (! interrupted) lastPeak.store (peak);
    }
}

// ------------------------------------------------------------- exporter --

void Exporter::estimate (const OfflineSettings& s, int64_t& frames, int& channels, double& rate)
{
    frames = 0; channels = 2; rate = 44100;
    if (s.data == nullptr) return;
    const auto view = makeView (*s.data, s.regionA, s.regionB);
    rate = view.sampleRate;
    frames = (int64_t) std::llround (view.len * s.stream.stretch);
    if (s.reverbPct > 0) frames += (int64_t) std::llround ((0.012 + juce::jmax (0.05f, s.sizeSec)) * rate);
    channels = (view.numChannels == 1 && ! s.stream.monoToStereo && s.reverbPct <= 0) ? 1 : 2;
}

bool Exporter::start (const OfflineSettings& s, const juce::File& file, int format, int bits)
{
    if (isThreadRunning() || s.data == nullptr) return false;
    job = s;
    job.stream.loop = false;
    outFile = file;
    fmt = format;
    bitDepth = bits;
    prog.store (0);
    st.store (Running);
    { const juce::ScopedLock sl (msgLock); msg = {}; }
    startThread (juce::Thread::Priority::normal);
    return true;
}

void Exporter::finish (State s, const juce::String& m)
{
    { const juce::ScopedLock sl (msgLock); msg = m; }
    st.store (s);
}

void Exporter::run()
{
    const auto view = makeView (*job.data, job.regionA, job.regionB);
    const double rate = view.sampleRate;
    int64_t frames; int channels; double r;
    estimate (job, frames, channels, r);
    if (frames * 2 > kMaxSamples) { finish (Failed, "output too long — select a shorter region or lower the stretch"); return; }

    StretchStream stream;
    stream.prepare (rate);
    stream.start (0.0, juce::Random::getSystemRandom().nextInt() | 1, job.stream.reverse, view.len);

    const size_t dryLen = (size_t) std::llround (view.len * job.stream.stretch) + (size_t) stream.grainSize();
    std::vector<float> L, R;
    try { L.assign (dryLen, 0.0f); R.assign (dryLen, 0.0f); }
    catch (...) { finish (Failed, "not enough memory"); return; }

    size_t done = 0;
    const int block = 8192;
    while (done < dryLen)
    {
        if (threadShouldExit()) { finish (Cancelled, "export cancelled"); return; }
        const int n = (int) std::min<size_t> (block, dryLen - done);
        const int got = stream.render (view, job.stream, L.data() + done, R.data() + done, n, 1.0f);
        done += (size_t) got;
        prog.store (0.85f * (float) done / (float) dryLen);
        if (got < n) break;
    }
    L.resize (done);
    R.resize (done);

    std::vector<float> outL, outR;
    if (job.reverbPct > 0)
    {
        std::vector<float> irL, irR;
        makeImpulse (rate, job.sizeSec, kIrSeed, irL, irR);
        const double a = job.reverbPct / 100.0;
        const float dry = (float) std::cos (a * kPi / 2), wet = (float) std::sin (a * kPi / 2);
        const size_t len = done + irL.size() - 1;
        try { outL.assign (len, 0.0f); outR.assign (len, 0.0f); }
        catch (...) { finish (Failed, "not enough memory"); return; }
        fftConvolve (L.data(), done, irL.data(), irL.size(), outL.data());
        if (threadShouldExit()) { finish (Cancelled, "export cancelled"); return; }
        prog.store (0.92f);
        fftConvolve (R.data(), done, irR.data(), irR.size(), outR.data());
        for (size_t i = 0; i < len; ++i)
        {
            outL[i] *= wet; outR[i] *= wet;
            if (i < done) { outL[i] += dry * L[i]; outR[i] += dry * R[i]; }
        }
        L.swap (outL);
        R.swap (outR);
    }
    if (threadShouldExit()) { finish (Cancelled, "export cancelled"); return; }
    prog.store (0.96f);

    float peak = 0;
    for (size_t i = 0; i < L.size(); ++i) peak = juce::jmax (peak, std::abs (L[i]), std::abs (R[i]));
    const float norm = job.normalize ? std::pow (10.0f, -0.5f / 20.0f) / juce::jmax (1e-9f, peak) : 1.0f;
    const float gain = norm * juce::Decibels::decibelsToGain (job.gainDb, -100.0f);

    std::vector<const float*> chans { L.data() };
    if (channels == 2) chans.push_back (R.data());
    juce::String err;
    if (! writeAudioFile (outFile, fmt, bitDepth, chans, L.size(), rate, gain, err)) { finish (Failed, err); return; }
    prog.store (1.0f);

    const float outDb = juce::Decibels::gainToDecibels (peak * gain, -100.0f);
    const auto sizeMB = juce::String ((double) outFile.getSize() / 1048576.0, 1) + " MB";
    if (outDb > 0.01f && bitDepth != 32)
        finish (Done, "saved " + outFile.getFileName() + " · clipped by " + juce::String (outDb, 1) + " dB (lower GAIN)");
    else
        finish (Done, "saved " + outFile.getFileName() + " · " + sizeMB);
}

} // namespace ss
