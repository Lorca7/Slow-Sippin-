#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace ss
{

/** v with exactly `decimals` places. (juce::String (v, 0) does NOT round: 0 means "default formatting".) */
inline juce::String fmtDec (double v, int decimals)
{
    return decimals <= 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
}

/** Everything a control row needs: parameter range + how the slider maps to it. */
struct ControlSpec
{
    enum Group { Engine, Output, Fx, Hidden, LfoRate };
    enum Scale { Linear, Log, Sqrt };

    const char* id;
    const char* label;
    const char* unit;
    float pMin, pMax;        // parameter range (number box accepts all of it)
    float sMin, sMax;        // slider range (may be narrower: "the number box accepts more")
    float def;
    int decimals;
    Scale scale;
    bool integer;
    bool engine;             // part of the stretch engine (red dot)
    Group group = Output;
    bool modulatable = true; // can an LFO be dropped on it
};

constexpr int kNumLfos = 8;

inline const std::vector<ControlSpec>& controlSpecs()
{
    using C = ControlSpec;
    static const std::vector<ControlSpec> specs {
        // ---- stretch engine
        { "stretch", "STRETCH", "x",  1.0f, 1000000.0f, 1.0f, 1000.0f, 20.0f, 2, C::Log,    false, true,  C::Engine },
        { "pitch",   "PITCH",   "st", -60.0f, 60.0f,  -48.0f, 48.0f,    0.0f, 1, C::Linear, false, true,  C::Engine },
        { "weave",   "WEAVE",   "",   1.0f, 16.0f,     1.0f, 16.0f,     4.0f, 0, C::Linear, true,  true,  C::Engine },
        { "scatter", "SCATTER", "%",  0.0f, 100.0f,    0.0f, 100.0f,  100.0f, 0, C::Linear, false, true,  C::Engine },
        { "drift",   "DRIFT",   "ms", 0.0f, 10000.0f,  0.0f, 1000.0f,   0.0f, 0, C::Sqrt,   false, true,  C::Engine },
        { "window",  "WINDOW",  "%",  -100.0f, 100.0f, -100.0f, 100.0f, 0.0f, 1, C::Linear, false, true,  C::Engine },
        // ---- output
        { "reverb",  "REVERB",  "%",  0.0f, 100.0f,    0.0f, 100.0f,    0.0f, 0, C::Linear, false, false, C::Output },
        { "size",    "SIZE",    "s",  0.1f, 30.0f,     0.2f, 20.0f,     2.5f, 1, C::Log,    false, false, C::Output, false },
        { "gain",    "GAIN",    "dB", -60.0f, 24.0f,  -24.0f, 24.0f,    0.0f, 1, C::Linear, false, false, C::Output },
        { "attack",  "ATTACK",  "ms", 0.0f, 20000.0f,  0.0f, 10000.0f, 40.0f, 0, C::Sqrt,   false, false, C::Output },
        { "release", "RELEASE", "ms", 0.0f, 30000.0f,  0.0f, 20000.0f, 1500.0f, 0, C::Sqrt, false, false, C::Output },
        // ---- effects: two slots (A, B), the XY pad drives the parameters chosen for its axes
        { "fxMix",    "FX MIX",    "%",  0.0f, 100.0f,   0.0f, 100.0f,    0.0f, 0, C::Linear, false, false, C::Fx },
        { "fxBal",    "A / B BALANCE", "%", 0.0f, 100.0f, 0.0f, 100.0f,  50.0f, 0, C::Linear, false, false, C::Fx },
        { "fxX",      "X",         "%",  0.0f, 100.0f,   0.0f, 100.0f,   50.0f, 0, C::Linear, false, false, C::Hidden },
        { "fxY",      "Y",         "%",  0.0f, 100.0f,   0.0f, 100.0f,   50.0f, 0, C::Linear, false, false, C::Hidden },
        { "chRate",   "RATE",      "Hz", 0.02f, 8.0f,    0.02f, 8.0f,    0.35f, 2, C::Log,    false, false, C::Fx },
        { "chDepth",  "DEPTH",     "%",  0.0f, 100.0f,   0.0f, 100.0f,   50.0f, 0, C::Linear, false, false, C::Fx },
        { "echoTime", "TIME",      "ms", 20.0f, 2000.0f, 20.0f, 2000.0f, 375.0f, 0, C::Log,   false, false, C::Fx },
        { "echoFb",   "FEEDBACK",  "%",  0.0f, 110.0f,   0.0f, 110.0f,   55.0f, 0, C::Linear, false, false, C::Fx },
        { "echoTone", "TONE",      "%",  0.0f, 100.0f,   0.0f, 100.0f,   45.0f, 0, C::Linear, false, false, C::Fx },
        { "fltCut",   "CUTOFF",    "Hz", 20.0f, 20000.0f, 20.0f, 20000.0f, 1200.0f, 0, C::Log, false, false, C::Fx },
        { "fltRes",   "RESONANCE", "%",  0.0f, 100.0f,   0.0f, 100.0f,   30.0f, 0, C::Linear, false, false, C::Fx },
        { "fltType",  "LP > BP > HP", "%", 0.0f, 100.0f, 0.0f, 100.0f,    0.0f, 0, C::Linear, false, false, C::Fx },
        { "spBlur",   "BLUR",      "%",  0.0f, 100.0f,   0.0f, 100.0f,   60.0f, 0, C::Linear, false, false, C::Fx },
        { "spShift",  "SHIFT",     "st", -24.0f, 24.0f, -24.0f, 24.0f,    0.0f, 1, C::Linear, false, false, C::Fx },
        { "spThin",   "THIN",      "%",  0.0f, 100.0f,   0.0f, 100.0f,    0.0f, 0, C::Linear, false, false, C::Fx },
        // ---- LFO rates
        { "lfo1Rate", "LFO 1", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 0.10f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo2Rate", "LFO 2", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 0.25f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo3Rate", "LFO 3", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 0.50f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo4Rate", "LFO 4", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 1.00f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo5Rate", "LFO 5", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 0.05f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo6Rate", "LFO 6", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 0.15f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo7Rate", "LFO 7", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 2.00f, 2, C::Log, false, false, C::LfoRate, false },
        { "lfo8Rate", "LFO 8", "Hz", 0.01f, 20.0f, 0.01f, 20.0f, 6.00f, 2, C::Log, false, false, C::LfoRate, false },
    };
    return specs;
}

inline const ControlSpec* findSpec (const juce::String& id)
{
    for (const auto& c : controlSpecs()) if (id == c.id) return &c;
    return nullptr;
}

struct ToggleSpec { const char* id; const char* label; bool def; };

inline const std::vector<ToggleSpec>& toggleSpecs()
{
    static const std::vector<ToggleSpec> specs {
        { "normalize",  "NORMALIZE",     true  },
        { "monoStereo", "MONO → STEREO", true  },
        { "reverse",    "REVERSE",       false },
        { "loop",       "LOOP",          false },
        { "keyTrack",   "KEY TRACK",     true  },
    };
    return specs;
}

// ------------------------------------------------------------- LFO shapes

enum LfoShape { Sine, Triangle, SawUp, SawDown, Square, SampleHold, Wander, kNumShapes };

inline juce::StringArray lfoShapeNames()
{
    return { "SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "S&H", "WANDER" };
}

inline const int kDefaultShapes[kNumLfos] = { Sine, Triangle, Wander, Sine, SawUp, Wander, SampleHold, Sine };

inline juce::String lfoRateId (int i)  { return "lfo" + juce::String (i + 1) + "Rate"; }
inline juce::String lfoShapeId (int i) { return "lfo" + juce::String (i + 1) + "Shape"; }

// ----------------------------------------------------- modulation targets

/** Everything an LFO can drive, in a fixed order the DSP code uses directly. */
enum Target
{
    tStretch, tPitch, tWeave, tScatter, tDrift, tWindow,
    tReverb, tGain, tAttack, tRelease,
    tFxMix, tFxX, tFxY, tFxBal, tChRate, tChDepth, tEchoTime, tEchoFb, tEchoTone,
    tFltCut, tFltRes, tFltType, tSpBlur, tSpShift, tSpThin,
    kNumTargets
};

inline const char* targetParamId (int t)
{
    static const char* ids[kNumTargets] = {
        "stretch", "pitch", "weave", "scatter", "drift", "window",
        "reverb", "gain", "attack", "release",
        "fxMix", "fxX", "fxY", "fxBal", "chRate", "chDepth", "echoTime", "echoFb", "echoTone",
        "fltCut", "fltRes", "fltType", "spBlur", "spShift", "spThin" };
    return ids[t];
}

inline int targetForParam (const juce::String& id)
{
    for (int t = 0; t < kNumTargets; ++t) if (id == targetParamId (t)) return t;
    return -1;
}

// ------------------------------------------------- effect slots + XY pad

enum FxType { FxChorus, FxEcho, FxFilter, FxSpectral, kNumFxTypes };

inline juce::StringArray fxTypeNames() { return { "CHORUS", "DUB ECHO", "FILTER", "SPECTRAL" }; }

/** The parameters of each effect, in display order. */
inline std::vector<int> fxTypeTargets (int type)
{
    switch (type)
    {
        case FxChorus:   return { tChRate, tChDepth };
        case FxEcho:     return { tEchoTime, tEchoFb, tEchoTone };
        case FxFilter:   return { tFltCut, tFltRes, tFltType };
        case FxSpectral: return { tSpBlur, tSpShift, tSpThin };
        default:         return {};
    }
}

inline int fxTypeOfTarget (int t)
{
    for (int k = 0; k < kNumFxTypes; ++k)
        for (int x : fxTypeTargets (k)) if (x == t) return k;
    return -1;
}

/** What an XY-pad axis can drive: choice index → Target (-1 = off). */
inline int padChoiceToTarget (int choice)
{
    static const int map[] = { -1, tFxBal, tChRate, tChDepth, tEchoTime, tEchoFb, tEchoTone,
                               tFltCut, tFltRes, tFltType, tSpBlur, tSpShift, tSpThin };
    return choice >= 0 && choice < (int) (sizeof (map) / sizeof (map[0])) ? map[choice] : -1;
}

inline int targetToPadChoice (int target)
{
    for (int c = 0; c < 13; ++c) if (padChoiceToTarget (c) == target) return c;
    return 0;
}

inline juce::StringArray padChoiceNames()
{
    return { "OFF", "A / B BALANCE", "CHORUS RATE", "CHORUS DEPTH", "ECHO TIME", "ECHO FEEDBACK", "ECHO TONE",
             "FILTER CUTOFF", "FILTER RESONANCE", "FILTER TYPE", "SPECTRAL BLUR", "SPECTRAL SHIFT", "SPECTRAL THIN" };
}

// ------------------------------------------------------- slider mapping

/** Slider position (0..1) ↔ value, using the slider range. */
inline double specToPos (const ControlSpec& c, double v)
{
    v = juce::jlimit ((double) c.sMin, (double) c.sMax, v);
    switch (c.scale)
    {
        case ControlSpec::Log:  return std::log (v / c.sMin) / std::log ((double) c.sMax / c.sMin);
        case ControlSpec::Sqrt: return std::sqrt ((v - c.sMin) / (c.sMax - c.sMin));
        default:                return (v - c.sMin) / (c.sMax - c.sMin);
    }
}

inline double specFromPos (const ControlSpec& c, double t)
{
    t = juce::jlimit (0.0, 1.0, t);
    switch (c.scale)
    {
        case ControlSpec::Log:  return c.sMin * std::pow ((double) c.sMax / c.sMin, t);
        case ControlSpec::Sqrt: return c.sMin + (c.sMax - c.sMin) * t * t;
        default:                return c.sMin + (c.sMax - c.sMin) * t;
    }
}

inline double specQuantize (const ControlSpec& c, double v)
{
    if (c.integer) return std::round (v);
    const double q = std::pow (10.0, c.decimals);
    return std::round (v * q) / q;
}

/** Host-facing range: log/sqrt shaped over the full parameter range. */
inline juce::NormalisableRange<float> specRange (const ControlSpec& c)
{
    const float lo = c.pMin, hi = c.pMax;
    if (c.scale == ControlSpec::Log)
        return { lo, hi,
                 [] (float a, float b, float t) { return a * std::pow (b / a, t); },
                 [] (float a, float b, float v) { return std::log (v / a) / std::log (b / a); },
                 nullptr };
    if (c.scale == ControlSpec::Sqrt)
        return { lo, hi,
                 [] (float a, float b, float t) { return a + (b - a) * t * t; },
                 [] (float a, float b, float v) { return std::sqrt (juce::jmax (0.0f, (v - a) / (b - a))); },
                 nullptr };
    return { lo, hi, c.integer ? 1.0f : 0.0f };
}

/** Host-visible name: FX parameters get their effect as prefix (CUTOFF → FILTER CUTOFF). */
inline juce::String hostName (const ControlSpec& c)
{
    const juce::String id (c.id);
    if (id.startsWith ("ch")) return "CHORUS " + juce::String (c.label);
    if (id.startsWith ("echo")) return "ECHO " + juce::String (c.label);
    if (id.startsWith ("flt")) return "FILTER " + juce::String::fromUTF8 (c.label);
    if (id.startsWith ("sp")) return "SPECTRAL " + juce::String (c.label);
    if (id == "fxX") return "PAD X";
    if (id == "fxY") return "PAD Y";
    if (id == "fxBal") return "FX A / B BALANCE";
    return juce::String::fromUTF8 (c.label);
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& c : controlSpecs())
    {
        auto attrs = juce::AudioParameterFloatAttributes()
                         .withLabel (c.unit)
                         .withStringFromValueFunction ([dec = c.decimals] (float v, int) { return fmtDec (v, dec); });
        const auto name = hostName (c);
        if (c.integer)
            layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { c.id, 1 }, name, (int) c.pMin, (int) c.pMax, (int) c.def));
        else
            layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { c.id, 1 }, name, specRange (c), c.def, attrs));
    }
    for (const auto& t : toggleSpecs())
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { t.id, 1 }, juce::String::fromUTF8 (t.label), t.def));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "fxA", 1 }, "FX SLOT A", fxTypeNames(), FxFilter));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "fxB", 1 }, "FX SLOT B", fxTypeNames(), FxEcho));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "fxRoute", 1 }, "FX ROUTING", juce::StringArray { "SERIES", "PARALLEL" }, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "padX", 1 }, "PAD X CONTROLS", padChoiceNames(), targetToPadChoice (tFltCut)));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "padY", 1 }, "PAD Y CONTROLS", padChoiceNames(), targetToPadChoice (tEchoFb)));
    for (int i = 0; i < kNumLfos; ++i)
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { lfoShapeId (i), 1 },
                                                                   "LFO " + juce::String (i + 1) + " SHAPE", lfoShapeNames(), kDefaultShapes[i]));
    return layout;
}

} // namespace ss
