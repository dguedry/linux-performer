#pragma once

#include <juce_core/juce_core.h>

namespace perf::LowLatency
{

/** Makes JUCE's "JACK" device type talk to PipeWire: loads PipeWire's libjack
    (normally only reachable through `pw-jack`) before JUCE looks for libjack.so.0.
    Returns true if PipeWire's JACK library was found and loaded. Skipped when a
    real jackd is running. */
bool preloadPipeWireJack();
bool pipeWireJackLoaded();

/** Requests a graph position for our JACK/PipeWire client: PIPEWIRE_LATENCY=quantum/rate.
    Takes effect when the audio device is (re)opened. */
void setRequestedQuantum (int quantum, int sampleRate);

/** True if this process may use SCHED_FIFO/RR (RLIMIT_RTPRIO > 0). Without it the
    audio threads run at normal priority and small buffers will drop out. */
bool realtimeSchedulingAvailable();
juce::String realtimeSchedulingHint();

/** PipeWire's session clock settings, as pw-metadata reports them. Empty = not set. */
struct ClockState { juce::String forceQuantum, forceRate, quantum, rate; };
ClockState readClock();
/** Pins the whole PipeWire graph to quantum/rate (clock.force-quantum / force-rate).
    Affects every application; meant for a dedicated live machine. */
bool forceClock (int quantum, int sampleRate);
/** Restores the force settings captured by readClock() (clears them if they were unset). */
bool restoreClock (const ClockState& previous);

} // namespace perf::LowLatency
