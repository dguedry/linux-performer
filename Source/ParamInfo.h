#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace perf
{

/** Description of one plugin parameter, as reported by the plugin host process. */
struct ParamInfo
{
    int index = 0;
    juce::String id;          // stable ID (VST3/LV2 parameter ID), or the index as text
    juce::String name;
    bool automatable = true;
    bool discrete = false;
    bool boolean = false;
    /** Positions the parameter has, or a large number when it is continuous.
        Plugins are unreliable about `boolean`, so this is the better signal:
        two steps is a switch whatever the plugin claims. */
    int numSteps = 0;
    float value = 0.0f;       // last known normalised value (0..1)
    /** VST3 plugins take no MIDI CCs directly: they publish one parameter per
        (MIDI channel, controller) and the host converts. When the plugin told us
        which pair this parameter stands for, midiChannel is 1..16 and
        midiController 0..127 for CCs, 128 aftertouch, 129 pitch bend, 130 program
        change. Otherwise 0 / -1. */
    int midiChannel = 0;
    int midiController = -1;
};

using ParamInfoList = std::vector<ParamInfo>;

} // namespace perf
