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
    float value = 0.0f;       // last known normalised value (0..1)
};

using ParamInfoList = std::vector<ParamInfo>;

} // namespace perf
