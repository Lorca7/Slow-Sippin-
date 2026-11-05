#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace ss
{

/** Everything a control row needs: parameter range + how the slider maps to it. */
struct ControlSpec
{
    const char* id;
    const char* label;
    const char* unit;
    float pMin, pMax;        // parameter range (number box accepts all of it)
    float sMin, sMax;        // slider range (may be narrower: "the number box accepts more")
    float def;
    int decimals;
    enum Scale { Linear, Log, Sqrt } scale;
    bool integer;
    bool engine;             // part of the stretch engine (red dot)
};

inline const std::vector<ControlSpec>& controlSpecs()
{
    static const std::vector<ControlSpec> specs {
        { "stretch", "STRETCH", "x",  1.0f, 1000000.0f, 1.0f, 1000.0f, 20.0f, 2, ControlSpec::Log,    false, true  },
        { "pitch",   "PITCH",   "st", -60.0f, 60.0f,  -48.0f, 48.0f,    0.0f, 1, ControlSpec::Linear, false, true  },
        { "weave",   "WEAVE",   "",   1.0f, 16.0f,     1.0f, 16.0f,     4.0f, 0, ControlSpec::Linear, true,  true  },
        { "scatter", "SCATTER", "%",  0.0f, 100.0f,    0.0f, 100.0f,  100.0f, 0, ControlSpec::Linear, false, true  },
        { "drift",   "DRIFT",   "ms", 0.0f, 10000.0f,  0.0f, 1000.0f,   0.0f, 0, ControlSpec::Sqrt,   false, true  },
        { "reverb",  "REVERB",  "%",  0.0f, 100.0f,    0.0f, 100.0f,    0.0f, 0, ControlSpec::Linear, false, false },
        { "size",    "SIZE",    "s",  0.1f, 30.0f,     0.2f, 20.0f,     2.5f, 1, ControlSpec::Log,    false, false },
        { "gain",    "GAIN",    "dB", -60.0f, 24.0f,  -24.0f, 24.0f,    0.0f, 1, ControlSpec::Linear, false, false },
        { "attack",  "ATTACK",  "ms", 0.0f, 20000.0f,  0.0f, 10000.0f, 40.0f, 0, ControlSpec::Sqrt,   false, false },
        { "release", "RELEASE", "ms", 0.0f, 30000.0f,  0.0f, 20000.0f, 1500.0f, 0, ControlSpec::Sqrt, false, false },
    };
    return specs;
}

struct ToggleSpec { const char* id; const char* label; bool def; };

inline const std::vector<ToggleSpec>& toggleSpecs()
{
    static const std::vector<ToggleSpec> specs {
        { "normalize",  "NORMALIZE",     true  },
        { "monoStereo", "MONO > STEREO", true  },
        { "reverse",    "REVERSE",       false },
        { "loop",       "LOOP",          false },
        { "keyTrack",   "KEY TRACK",     true  },
    };
    return specs;
}

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

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& c : controlSpecs())
    {
        auto attrs = juce::AudioParameterFloatAttributes()
                         .withLabel (c.unit)
                         .withStringFromValueFunction ([dec = c.decimals] (float v, int) { return juce::String (v, dec); });
        if (c.integer)
            layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { c.id, 1 }, c.label, (int) c.pMin, (int) c.pMax, (int) c.def));
        else
            layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { c.id, 1 }, c.label, specRange (c), c.def, attrs));
    }
    for (const auto& t : toggleSpecs())
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { t.id, 1 }, t.label, t.def));
    return layout;
}

} // namespace ss
