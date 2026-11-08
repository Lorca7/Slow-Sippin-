#include "Offline.h"

namespace ss
{

static juce::String U8 (const char* s) { return juce::String::fromUTF8 (s); }

static constexpr uint32_t kIrSeed = 90210;

SourceView makeView (const SampleData& d, int a, int b) noexcept
{
    return regionView (d, a, b, 0.0f);
}

// ============================================================ PcmWriter

static void put64 (juce::OutputStream& o, uint64_t v) { o.writeInt64 ((juce::int64) v); }
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

bool PcmWriter::open (const juce::File& f, int format, int bitDepth, int channels, uint64_t frames, double sampleRate, juce::String& error)
{
    fmt = format;
    bits = bitDepth;
    ch = channels;
    bps = bits / 8;
    const uint64_t dataLen = frames * (uint64_t) ch * (uint64_t) bps;
    const bool big = dataLen + 1024 > 0xFFFFFFFFull;
    if (big && fmt == 1) { error = "AIFF files can't be larger than 4 GB - choose WAV for a render this long"; return false; }

    f.deleteFile();
    out = std::make_unique<juce::FileOutputStream> (f, 1 << 20);
    if (! out->openedOk()) { error = "cannot write " + f.getFullPathName(); out.reset(); return false; }
    auto& o = *out;
    const bool isFloat = bits == 32;
    const int sr = (int) std::lround (sampleRate);

    if (fmt == 0)
    {
        if (big)
        {
            // RF64 (EBU Tech 3306): WAV with 64-bit sizes in a ds64 chunk
            o.write ("RF64", 4); put32 (o, 0xFFFFFFFFu, true); o.write ("WAVE", 4);
            o.write ("ds64", 4); put32 (o, 28, true);
            put64 (o, 4 + (8 + 28) + (8 + 16) + 8 + dataLen);   // RIFF size
            put64 (o, dataLen);
            put64 (o, frames);
            put32 (o, 0, true);
        }
        else
        {
            o.write ("RIFF", 4); put32 (o, (uint32_t) (36 + dataLen), true); o.write ("WAVE", 4);
        }
        o.write ("fmt ", 4); put32 (o, 16, true);
        put16 (o, isFloat ? 3 : 1, true);
        put16 (o, (uint16_t) ch, true);
        put32 (o, (uint32_t) sr, true);
        put32 (o, (uint32_t) (sr * ch * bps), true);
        put16 (o, (uint16_t) (ch * bps), true);
        put16 (o, (uint16_t) bits, true);
        o.write ("data", 4); put32 (o, big ? 0xFFFFFFFFu : (uint32_t) dataLen, true);
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
    return ! o.getStatus().failed();
}

bool PcmWriter::write (const float* x, size_t frames, float gain)
{
    if (out == nullptr) return false;
    const bool wav = fmt == 0;
    buf.resize (frames * (size_t) ch * (size_t) bps);
    uint8_t* p = buf.data();
    const size_t n = frames * (size_t) ch;
    for (size_t i = 0; i < n; ++i)
    {
        const double v = (double) x[i] * gain;
        if (bits == 32)
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
    return out->write (buf.data(), buf.size());
}

bool PcmWriter::close (juce::String& error)
{
    if (out == nullptr) return false;
    out->flush();
    const bool ok = ! out->getStatus().failed();
    if (! ok) error = "disk full or write error: " + out->getStatus().getErrorMessage();
    out.reset();
    return ok;
}

bool writeAudioFile (const juce::File& f, int format, int bits, const std::vector<const float*>& chans,
                     size_t frames, double sampleRate, float gain, juce::String& error)
{
    PcmWriter w;
    const int ch = (int) chans.size();
    if (! w.open (f, format, bits, ch, frames, sampleRate, error)) return false;
    std::vector<float> il;
    for (size_t f0 = 0; f0 < frames; f0 += 65536)
    {
        const size_t k = std::min<size_t> (65536, frames - f0);
        il.resize (k * (size_t) ch);
        for (size_t i = 0; i < k; ++i)
            for (int c = 0; c < ch; ++c) il[i * (size_t) ch + (size_t) c] = chans[(size_t) c][f0 + i];
        if (! w.write (il.data(), k, gain)) { error = "write error"; return false; }
    }
    return w.close (error);
}

// ====================================================== StreamConvolver

void StreamConvolver::prepare (const std::vector<float>& ir)
{
    const int hLen = juce::jmax (1, (int) ir.size());
    n = 1 << 15;
    while (n < 2 * hLen) n <<= 1;
    B = n - hLen + 1;
    H = n / 2 + 1;
    fft.prepare (n);
    hr.assign ((size_t) H, 0.0); hi.assign ((size_t) H, 0.0);
    xr.assign ((size_t) H, 0.0); xi.assign ((size_t) H, 0.0);
    buf.assign ((size_t) n, 0.0);
    acc.assign ((size_t) n, 0.0);
    for (int i = 0; i < n; ++i) buf[(size_t) i] = i < (int) ir.size() ? ir[(size_t) i] : 0.0;
    fft.forward (buf.data(), hr.data(), hi.data());
}

void StreamConvolver::process (const float* in, float* out)
{
    for (int i = 0; i < n; ++i) buf[(size_t) i] = i < B ? in[i] : 0.0;
    fft.forward (buf.data(), xr.data(), xi.data());
    for (int k = 0; k < H; ++k)
    {
        const double r = xr[(size_t) k] * hr[(size_t) k] - xi[(size_t) k] * hi[(size_t) k];
        const double im = xr[(size_t) k] * hi[(size_t) k] + xi[(size_t) k] * hr[(size_t) k];
        xr[(size_t) k] = r; xi[(size_t) k] = im;
    }
    fft.inverse (xr.data(), xi.data(), buf.data());
    for (int i = 0; i < n; ++i) acc[(size_t) i] += buf[(size_t) i];
    for (int i = 0; i < B; ++i) out[i] = (float) acc[(size_t) i];
    std::copy (acc.begin() + B, acc.end(), acc.begin());
    std::fill (acc.end() - B, acc.end(), 0.0);
}

// ============================================================= Analyzer

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

// ============================================================= Exporter

static int irLength (double rate, float size)
{
    return (int) std::lround (0.012 * rate) + juce::jmax (1, (int) std::lround (juce::jmax (0.05f, size) * rate));
}

void Exporter::estimate (const OfflineSettings& s, int64_t& frames, int& channels, double& rate)
{
    frames = 0; channels = 2; rate = 44100;
    if (s.data == nullptr) return;
    const auto view = makeView (*s.data, s.regionA, s.regionB);
    rate = view.sampleRate;
    frames = (int64_t) std::llround (view.len * s.stream.stretch) + windowSizeFor (rate) / 2;
    if (s.reverbPct > 0) frames += irLength (rate, s.sizeSec) - 1;
    channels = (view.numChannels == 1 && ! s.stream.monoToStereo && s.reverbPct <= 0 && s.base[tFxMix] <= 0) ? 1 : 2;
}

bool Exporter::start (const OfflineSettings& s, const juce::File& file, int format, int bits)
{
    if (isThreadRunning() || s.data == nullptr) return false;
    job = s;
    if (job.mod == nullptr)
    {
        // no modulation data supplied: take everything from the plain settings, effects off
        for (int t = 0; t < kNumTargets; ++t)
            if (auto* sp = findSpec (targetParamId (t))) job.base[(size_t) t] = sp->def;
        job.base[tStretch] = (float) job.stream.stretch;
        job.base[tPitch] = (float) job.stream.pitchSemis;
        job.base[tWeave] = (float) job.stream.weave;
        job.base[tScatter] = (float) job.stream.scatter * 100.0f;
        job.base[tDrift] = (float) job.stream.driftMs;
        job.base[tWindow] = 0;
        job.base[tReverb] = job.reverbPct;
        job.base[tGain] = job.gainDb;
        job.base[tFxMix] = 0;
    }
    job.stream.loop = false;
    { const juce::ScopedLock sl (msgLock); outFile = file; msg = {}; }
    fmt = format;
    bitDepth = bits;
    prog.store (0);
    st.store (Running);
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
    int64_t estFrames; int ch; double r;
    estimate (job, estFrames, ch, r);
    if (tooLong (estFrames, rate)) { finish (Failed, "longer than 12 hours - select a shorter region or lower the stretch"); return; }

    // disk space: temporary float file + the final file
    const auto dir = outFile.getParentDirectory();
    const int64_t need = estFrames * ch * 4 + estFrames * ch * (bitDepth / 8) + (64 << 20);
    const int64_t free = dir.getBytesFreeOnVolume();
    if (free > 0 && free < need)
    {
        finish (Failed, "not enough disk space (needs " + juce::String ((double) need / 1073741824.0, 1) + " GB free)");
        return;
    }

    const juce::File temp = dir.getChildFile ("." + outFile.getFileName() + ".slowsippin-tmp");
    struct Cleanup { juce::File f; ~Cleanup() { f.deleteFile(); } } cleanup { temp };

    // ---------------- pass 1: render → temporary float file, measure peak
    // stretch (modulated) → effects (modulated) → reverb (wet modulated) → gain modulation.
    // The file's base GAIN is applied in pass 2, after NORMALIZE, as when playing.
    float peak = 0;
    uint64_t written = 0, lastLoud = 0;
    {
        temp.deleteFile();
        juce::FileOutputStream tmp (temp, 1 << 20);
        if (! tmp.openedOk()) { finish (Failed, "cannot write to " + dir.getFullPathName()); return; }

        auto modv = [this] (int t, const float* lfo)
        {
            return job.mod != nullptr ? job.mod->apply (t, job.base[(size_t) t], lfo, job.routes) : job.base[(size_t) t];
        };
        auto routed = [this] (int t) { for (const auto& r : job.routes) if (r.lfo >= 0 && r.target == t) return true; return false; };

        StretchStream stream;
        stream.prepare (rate);
        stream.start (0.0, (uint32_t) juce::Random::getSystemRandom().nextInt() | 1u, job.stream.reverse, view.len);
        FxChain fx;
        fx.prepare (rate, 256);

        const bool rev = job.base[tReverb] > 0 || routed (tReverb);
        StreamConvolver convL, convR;
        if (rev)
        {
            std::vector<float> irL, irR;
            makeImpulse (rate, job.sizeSec, kIrSeed, irL, irR);
            convL.prepare (irL);
            convR.prepare (irR);
        }
        const int block = rev ? convL.blockSize() : 65536;
        const int sub = 256;
        const float baseGain = juce::Decibels::decibelsToGain (job.base[tGain], -100.0f);
        std::vector<float> L ((size_t) block), R ((size_t) block), wL ((size_t) block), wR ((size_t) block);
        std::vector<float> wetA ((size_t) block), gainA ((size_t) block);
        std::vector<float> il ((size_t) block * (size_t) ch);
        const int64_t maxFrames = (int64_t) (kMaxSeconds * rate);
        const int64_t maxTail = (int64_t) ((60.0 + 3.0 * job.sizeSec) * rate);
        bool engineDone = false;
        int64_t afterEnd = 0;

        while (true)
        {
            if (threadShouldExit()) { finish (Cancelled, "export cancelled"); return; }
            std::fill (L.begin(), L.end(), 0.0f);
            std::fill (R.begin(), R.end(), 0.0f);
            for (int o = 0; o < block; o += sub)
            {
                const int n = std::min (sub, block - o);
                float lfo[kNumLfos];
                ModSystem::lfoValuesOffline ((double) (written + (uint64_t) o) / rate, job.lfoRate, job.lfoShape, lfo);
                if (! engineDone)
                {
                    StreamSettings st = job.stream;
                    st.stretch = modv (tStretch, lfo);
                    st.pitchSemis = modv (tPitch, lfo);
                    st.weave = (int) modv (tWeave, lfo);
                    st.scatter = modv (tScatter, lfo) / 100.0f;
                    st.driftMs = modv (tDrift, lfo);
                    const auto v = regionView (*job.data, job.regionA, job.regionB, modv (tWindow, lfo));
                    if (stream.render (v, st, L.data() + o, R.data() + o, n, job.preGain) < n) engineDone = true;
                }
                static const ModSystem noRoutes;
                const auto fp = makeFxParams (job.fx, job.mod != nullptr ? *job.mod : noRoutes, job.base.data(), lfo, &job.routes);
                fx.process (L.data() + o, R.data() + o, n, fp);
                const float wet = modv (tReverb, lfo) / 100.0f;
                const float g = juce::Decibels::decibelsToGain (modv (tGain, lfo), -100.0f) / juce::jmax (1.0e-6f, baseGain);
                std::fill (wetA.begin() + o, wetA.begin() + o + n, wet);
                std::fill (gainA.begin() + o, gainA.begin() + o + n, g);
            }
            if (rev)
            {
                convL.process (L.data(), wL.data());
                convR.process (R.data(), wR.data());
            }
            float blockPeak = 0;
            for (int i = 0; i < block; ++i)
            {
                float l = L[(size_t) i], r = R[(size_t) i];
                if (rev) { l += wetA[(size_t) i] * wL[(size_t) i]; r += wetA[(size_t) i] * wR[(size_t) i]; }
                l *= gainA[(size_t) i];
                r *= gainA[(size_t) i];
                const float a = std::max (std::abs (l), std::abs (r));
                if (a > 1.0e-5f) lastLoud = written + (uint64_t) i + 1;
                blockPeak = std::max (blockPeak, a);
                peak = std::max (peak, a);
                if (ch == 2) { il[(size_t) (2 * i)] = l; il[(size_t) (2 * i + 1)] = r; }
                else il[(size_t) i] = l;
            }
            if (! tmp.write (il.data(), (size_t) block * (size_t) ch * sizeof (float))) { finish (Failed, "disk full or write error"); return; }
            written += (uint64_t) block;
            prog.store (0.9f * (float) juce::jmin (1.0, (double) written / (double) juce::jmax<int64_t> (1, estFrames)));
            if (engineDone)
            {
                afterEnd += block;
                if (blockPeak < 1.0e-6f || afterEnd > maxTail) break;      // effect / reverb tails have died away
            }
            if ((int64_t) written > maxFrames) break;
        }
        tmp.flush();
        if (tmp.getStatus().failed()) { finish (Failed, "disk full or write error"); return; }
    }
    written = juce::jmax<uint64_t> (1, lastLoud);     // trim trailing silence

    // ---------------- pass 2: normalize + gain → final format
    const float norm = job.normalize ? std::pow (10.0f, -0.5f / 20.0f) / juce::jmax (1e-9f, peak) : 1.0f;
    const float gain = norm * juce::Decibels::decibelsToGain (job.gainDb, -100.0f);
    {
        juce::String err;
        PcmWriter w;
        if (! w.open (outFile, fmt, bitDepth, ch, written, rate, err)) { finish (Failed, err); return; }
        juce::FileInputStream in (temp);
        if (! in.openedOk()) { finish (Failed, "cannot read temporary file"); return; }
        std::vector<float> chunk ((size_t) 65536 * (size_t) ch);
        uint64_t done = 0;
        while (done < written)
        {
            if (threadShouldExit()) { w.close (err); outFile.deleteFile(); finish (Cancelled, "export cancelled"); return; }
            const size_t k = (size_t) std::min<uint64_t> (65536, written - done);
            const size_t bytes = k * (size_t) ch * sizeof (float);
            if ((size_t) in.read (chunk.data(), (int) bytes) != bytes) { w.close (err); finish (Failed, "read error"); return; }
            if (! w.write (chunk.data(), k, gain)) { w.close (err); finish (Failed, "disk full or write error"); return; }
            done += k;
            prog.store (0.9f + 0.1f * (float) ((double) done / (double) written));
        }
        if (! w.close (err)) { finish (Failed, err); return; }
    }
    prog.store (1.0f);

    const float outDb = juce::Decibels::gainToDecibels (peak * gain, -100.0f);
    const auto size = (double) outFile.getSize();
    const auto sizeTxt = size > 1073741824.0 ? juce::String (size / 1073741824.0, 2) + " GB" : juce::String (size / 1048576.0, 1) + " MB";
    if (outDb > 0.01f && bitDepth != 32)
        finish (Done, "saved " + outFile.getFileName() + U8 (" · clipped by ") + juce::String (outDb, 1) + " dB (lower GAIN)");
    else
        finish (Done, "saved " + outFile.getFileName() + U8 (" · ") + sizeTxt);
}

} // namespace ss
