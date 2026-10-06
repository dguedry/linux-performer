#include "ClapPluginFormat.h"

#include <clap/clap.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace perf
{

using namespace juce;

class ClapInstance;

namespace
{
    /* "<path>|<clap plugin id>". A pipe because a CLAP id is a reverse-DNS
       string ("com.u-he.diva") and a path is a path: neither contains one. */
    constexpr char kIdSeparator = '|';

    /** A loaded .clap bundle: the shared object plus its entry point.

        Closing the library runs the plugin's deinit, so this is kept alive for
        exactly as long as it is being used and no longer. Scanning loads and
        unloads around each bundle; an instance will hold one open for its
        lifetime. */
    class Bundle
    {
    public:
        static std::shared_ptr<Bundle> open (const File& file)
        {
            if (! file.exists()) return {};

            /* RTLD_LOCAL so one plugin's symbols cannot satisfy another's
               undefined ones -- two bundles built against different versions of
               the same library would otherwise resolve to whichever loaded
               first, and crash in ways that look like the second plugin's
               fault. */
            auto* handle = ::dlopen (file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr) return {};

            auto* entry = reinterpret_cast<const clap_plugin_entry_t*> (::dlsym (handle, "clap_entry"));
            if (entry == nullptr || entry->init == nullptr || entry->get_factory == nullptr)
            {
                ::dlclose (handle);
                return {};
            }

            // Not a CLAP we understand: refuse rather than guess at its ABI.
            if (! clap_version_is_compatible (entry->clap_version))
            {
                ::dlclose (handle);
                return {};
            }

            if (! entry->init (file.getFullPathName().toRawUTF8()))
            {
                ::dlclose (handle);
                return {};
            }

            return std::shared_ptr<Bundle> (new Bundle (handle, entry));
        }

        ~Bundle()
        {
            if (entry != nullptr && entry->deinit != nullptr) entry->deinit();
            if (handle != nullptr) ::dlclose (handle);
        }

        const clap_plugin_factory_t* factory() const
        {
            if (entry == nullptr) return nullptr;
            return static_cast<const clap_plugin_factory_t*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
        }

    private:
        Bundle (void* h, const clap_plugin_entry_t* e) : handle (h), entry (e) {}

        void* handle = nullptr;
        const clap_plugin_entry_t* entry = nullptr;

        JUCE_DECLARE_NON_COPYABLE (Bundle)
    };

    /** Fills in a JUCE description from a CLAP descriptor. */
    PluginDescription describe (const clap_plugin_descriptor_t& d, const File& file)
    {
        PluginDescription desc;
        desc.pluginFormatName = ClapPluginFormat::getFormatName();
        desc.name             = String::fromUTF8 (d.name != nullptr ? d.name : "");
        desc.descriptiveName  = String::fromUTF8 (d.description != nullptr ? d.description : "");
        desc.manufacturerName = String::fromUTF8 (d.vendor != nullptr ? d.vendor : "");
        desc.version          = String::fromUTF8 (d.version != nullptr ? d.version : "");
        desc.fileOrIdentifier = ClapPluginFormat::makeIdentifier (file.getFullPathName(),
                                                                  String::fromUTF8 (d.id != nullptr ? d.id : ""));
        desc.lastFileModTime  = file.getLastModificationTime();
        desc.lastInfoUpdateTime = Time::getCurrentTime();

        if (desc.name.isEmpty()) desc.name = file.getFileNameWithoutExtension();

        /* A CLAP says what it is in a null-terminated list of feature strings.
           "instrument" is the one that decides whether it can be a slot's
           instrument or only an effect. */
        bool isInstrument = false;
        StringArray features;
        if (d.features != nullptr)
            for (const char* const* f = d.features; *f != nullptr; ++f)
            {
                const String feature (String::fromUTF8 (*f));
                features.add (feature);
                if (feature == CLAP_PLUGIN_FEATURE_INSTRUMENT
                    || feature == CLAP_PLUGIN_FEATURE_SYNTHESIZER)
                    isInstrument = true;
            }

        desc.isInstrument = isInstrument;
        desc.category     = features.joinIntoString (", ");

        /* A plausible channel count for the list. CLAP reports its real port
           layout only once instantiated, so these are corrected when it is
           actually loaded rather than guessed at harder here. */
        desc.numInputChannels  = isInstrument ? 0 : 2;
        desc.numOutputChannels = 2;

        /* The id is what identifies a plugin across machines, so it is what the
           unique id is derived from -- a path would break the moment someone
           installed to a different prefix. */
        desc.uniqueId = desc.deprecatedUid = (int) String::fromUTF8 (d.id != nullptr ? d.id : "").hashCode();
        return desc;
    }
}


//==============================================================================
/** One parameter of a CLAP plugin.

    CLAP parameters carry plain values in their own range; JUCE wants 0..1. The
    conversion lives here so the rest of Performer -- MIDI learn, mappings,
    the tablet -- sees parameters exactly as it does for VST3. */
class ClapParameter final : public HostedAudioProcessorParameter
{
public:
    ClapParameter (ClapInstance& o, const clap_param_info_t& info, int index)
        : owner (o), id (info.id), idx (index),
          paramName (String::fromUTF8 (info.name)),
          minValue (info.min_value), maxValue (info.max_value),
          defaultValue (info.default_value),
          stepped ((info.flags & CLAP_PARAM_IS_STEPPED) != 0)
    {
    }

    String getParameterID() const override      { return String ((int64) id); }
    String getName (int maximumLength) const override { return paramName.substring (0, maximumLength); }
    String getLabel() const override            { return {}; }
    bool isDiscrete() const override            { return stepped; }
    bool isBoolean() const override             { return stepped && (maxValue - minValue) == 1.0; }

    int getNumSteps() const override
    {
        if (! stepped) return AudioProcessor::getDefaultNumParameterSteps();
        const auto span = maxValue - minValue;
        return span > 0.0 && span < 10000.0 ? (int) span + 1 : AudioProcessor::getDefaultNumParameterSteps();
    }

    float getValue() const override             { return toNormalised (currentPlain.load()); }
    float getDefaultValue() const override      { return toNormalised (defaultValue); }
    void setValue (float newValue) override;
    String getText (float normalised, int maximumLength) const override;
    float getValueForText (const String& text) const override;

    /** Called from the audio thread when the plugin reports a change of its own. */
    void setFromPlugin (double plain)
    {
        currentPlain.store (plain);
        sendValueChangedMessageToListeners (toNormalised (plain));
    }

    double toPlain (float normalised) const
    {
        return minValue + jlimit (0.0, 1.0, (double) normalised) * (maxValue - minValue);
    }

    clap_id getClapId() const { return id; }

private:
    float toNormalised (double plain) const
    {
        const auto span = maxValue - minValue;
        return span > 0.0 ? (float) jlimit (0.0, 1.0, (plain - minValue) / span) : 0.0f;
    }

    ClapInstance& owner;
    const clap_id id;
    const int idx;
    const String paramName;
    const double minValue, maxValue, defaultValue;
    const bool stepped;
    std::atomic<double> currentPlain { 0.0 };

    JUCE_DECLARE_NON_COPYABLE (ClapParameter)
};

//==============================================================================
/** A CLAP plugin, presented to JUCE as an AudioPluginInstance.

    The host side of CLAP is a set of callbacks the plugin may call, some from
    the audio thread and some only from the main thread; the split matters,
    because doing main-thread work from the audio callback is how a host gets
    dropouts. Everything here that the plugin can call at any time is either
    lock-free or deferred.
*/
class ClapInstance final : public AudioPluginInstance
{
public:
    ClapInstance (std::shared_ptr<Bundle> b, const clap_plugin_t* p, PluginDescription d)
        : bundle (std::move (b)), plugin (p), description (std::move (d))
    {
        host.host_data = this;
        host.clap_version = CLAP_VERSION;
        host.name = "Performer";
        host.vendor = "dguedry";
        host.url = "https://github.com/dguedry/linux-performer";
        host.version = "1.0";
        host.get_extension = &hostGetExtension;
        host.request_restart = &hostRequestRestart;
        host.request_process = &hostRequestProcess;
        host.request_callback = &hostRequestCallback;
    }

    ~ClapInstance() override
    {
        if (plugin != nullptr)
        {
            if (active) { plugin->stop_processing (plugin); plugin->deactivate (plugin); }
            plugin->destroy (plugin);
        }
    }

    /** Reads the plugin's ports and parameters. Must run before the plugin is used. */
    bool initialise (double sampleRate, int blockSize, String& error);

    //==============================================================================
    const String getName() const override                   { return description.name; }
    double getTailLengthSeconds() const override            { return 0.0; }
    bool acceptsMidi() const override                       { return hasNoteInput; }
    bool producesMidi() const override                      { return false; }

    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void processBlock (AudioBuffer<float>&, MidiBuffer&) override;

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return gui != nullptr; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const String getProgramName (int) override              { return {}; }
    void changeProgramName (int, const String&) override    {}

    void getStateInformation (MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    void fillInPluginDescription (PluginDescription& d) const override { d = description; }

    /** Queued by a parameter being set from the UI; drained into the plugin at
        the top of the next block, because that is the only place CLAP allows
        parameter events to be delivered.

        Lock-free on purpose: the audio thread drains this, and a knob being
        dragged must never be able to block an audio callback. If the queue is
        full the write is dropped rather than waiting -- a lost intermediate
        value during a fast drag is invisible, a dropout is not, and the final
        value of any gesture still arrives. */
    void queueParameterChange (clap_id id, double plainValue)
    {
        const auto w = queueWrite.load (std::memory_order_relaxed);
        const auto next = (w + 1) % queueCapacity;
        if (next == queueRead.load (std::memory_order_acquire))
            return;                      // full; drop this one

        queue[w] = { id, plainValue };
        queueWrite.store (next, std::memory_order_release);
    }

    const clap_plugin_t* getPlugin() const { return plugin; }

    /** The host struct a plugin is created with; it must outlive the plugin. */
    const clap_host_t* hostStruct() const { return &host; }

    /** Takes ownership of the plugin the factory made for this host struct. */
    void adopt (const clap_plugin_t* p) { plugin = p; }

private:
    //==============================================================================
    // Host callbacks. `request_*` may arrive from any thread.
    static ClapInstance* self (const clap_host_t* h) { return static_cast<ClapInstance*> (h->host_data); }

    static const void* CLAP_ABI hostGetExtension (const clap_host_t*, const char*)
    {
        /* No host extensions offered yet. A plugin asking for one it cannot have
           must get null, which every plugin handles: it is the normal answer for
           a host that does not implement something. */
        return nullptr;
    }

    static void CLAP_ABI hostRequestRestart (const clap_host_t* h)  { self (h)->restartRequested.store (true); }
    static void CLAP_ABI hostRequestProcess (const clap_host_t*)    {}
    static void CLAP_ABI hostRequestCallback (const clap_host_t* h) { self (h)->callbackRequested.store (true); }

    //==============================================================================
    std::shared_ptr<Bundle> bundle;
    const clap_plugin_t* plugin = nullptr;
    PluginDescription description;
    clap_host_t host {};

    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    const clap_plugin_audio_ports_t* audioPorts = nullptr;
    const clap_plugin_note_ports_t* notePorts = nullptr;

    bool active = false, processing = false, hasNoteInput = false;
    bool noteDialectMidi = true;         // false = CLAP's own note events
    int mainInChannels = 0, mainOutChannels = 2;

    Array<ClapParameter*> clapParams;    // owned by AudioProcessor
    std::atomic<bool> restartRequested { false }, callbackRequested { false };
    int64_t steadyTime = 0;

    /* Single-producer/single-consumer ring: the message thread writes, the
       audio thread reads. Capacity is generous enough for every parameter of
       a large plugin to be re-sent in one go, which is what a reactivation
       does. */
    struct PendingParam { clap_id id; double plain; };
    static constexpr int queueCapacity = 4096;
    std::array<PendingParam, queueCapacity> queue {};
    std::atomic<int> queueWrite { 0 }, queueRead { 0 };

    /* Event scratch for one process call. Lives here, not on the stack, so its
       storage is reserved once rather than allocated per block. */
    struct Events
    {
        std::vector<clap_event_header_t*> list;
        std::vector<clap_event_param_value_t> paramStore;
        std::vector<clap_event_note_t> noteStore;
        std::vector<clap_event_midi_t> midiStore;
    } events;

    // Scratch buffers for the process call, sized in prepareToPlay.
    std::vector<float*> inPtrs, outPtrs;
    AudioBuffer<float> silentIn;

    friend class ClapParameter;
    friend class ClapEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClapInstance)
};

//==============================================================================
void ClapParameter::setValue (float newValue)
{
    const auto plain = toPlain (newValue);
    currentPlain.store (plain);
    owner.queueParameterChange (id, plain);
}

String ClapParameter::getText (float normalised, int maximumLength) const
{
    if (owner.params != nullptr && owner.params->value_to_text != nullptr)
    {
        char buf[CLAP_NAME_SIZE] = {};
        if (owner.params->value_to_text (owner.getPlugin(), id, toPlain (normalised), buf, sizeof (buf)))
            return String::fromUTF8 (buf).substring (0, maximumLength);
    }
    return String (toPlain (normalised), 2).substring (0, maximumLength);
}

float ClapParameter::getValueForText (const String& text) const
{
    if (owner.params != nullptr && owner.params->text_to_value != nullptr)
    {
        double plain = 0.0;
        if (owner.params->text_to_value (owner.getPlugin(), id, text.toRawUTF8(), &plain))
            return toNormalised (plain);
    }
    return toNormalised (text.getDoubleValue());
}

//==============================================================================
bool ClapInstance::initialise (double sampleRate, int blockSize, String& error)
{
    if (plugin == nullptr) { error = "no plugin"; return false; }
    if (! plugin->init (plugin)) { error = "the plugin refused to initialise"; return false; }

    params     = static_cast<const clap_plugin_params_t*>      (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    state      = static_cast<const clap_plugin_state_t*>       (plugin->get_extension (plugin, CLAP_EXT_STATE));
    audioPorts = static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
    notePorts  = static_cast<const clap_plugin_note_ports_t*>  (plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS));

    /* The GUI extension is only useful if the plugin can embed in an X11
       window; a floating-window-only plugin would need a different path, and
       claiming hasEditor() for one would give an empty frame. */
    if (auto* g = static_cast<const clap_plugin_gui_t*> (plugin->get_extension (plugin, CLAP_EXT_GUI)))
        if (g->is_api_supported != nullptr && g->is_api_supported (plugin, CLAP_WINDOW_API_X11, false))
            gui = g;

    // --- audio ports: the main ones decide the bus layout -----------------
    if (audioPorts != nullptr)
    {
        clap_audio_port_info_t info {};
        if (audioPorts->count (plugin, true) > 0 && audioPorts->get (plugin, 0, true, &info))
            mainInChannels = (int) info.channel_count;
        if (audioPorts->count (plugin, false) > 0 && audioPorts->get (plugin, 0, false, &info))
            mainOutChannels = (int) info.channel_count;
    }
    mainOutChannels = jmax (1, mainOutChannels);

    setPlayConfigDetails (mainInChannels, mainOutChannels, sampleRate, blockSize);

    // --- note ports: can it be played, and in which dialect? ---------------
    if (notePorts != nullptr && notePorts->count (plugin, true) > 0)
    {
        clap_note_port_info_t np {};
        if (notePorts->get (plugin, 0, true, &np))
        {
            hasNoteInput = true;
            /* Prefer CLAP's own note events: they carry note ids and per-note
               expression that MIDI 1.0 cannot. Fall back to MIDI when that is
               all the port accepts. */
            noteDialectMidi = (np.supported_dialects & CLAP_NOTE_DIALECT_CLAP) == 0;
        }
    }

    // --- parameters --------------------------------------------------------
    if (params != nullptr && params->count != nullptr)
    {
        const uint32_t n = params->count (plugin);
        for (uint32_t i = 0; i < n; ++i)
        {
            clap_param_info_t info {};
            if (! params->get_info (plugin, i, &info)) continue;

            auto* cp = new ClapParameter (*this, info, (int) i);
            clapParams.add (cp);
            addHostedParameter (std::unique_ptr<HostedAudioProcessorParameter> (cp));

            double value = info.default_value;
            if (params->get_value != nullptr) params->get_value (plugin, info.id, &value);
            cp->setFromPlugin (value);
        }
    }

    description.numInputChannels  = mainInChannels;
    description.numOutputChannels = mainOutChannels;
    return true;
}

void ClapInstance::prepareToPlay (double sampleRate, int blockSize)
{
    if (plugin == nullptr) return;

    if (active) { if (processing) plugin->stop_processing (plugin); plugin->deactivate (plugin); }
    active = processing = false;

    setRateAndBufferSizeDetails (sampleRate, blockSize);

    if (! plugin->activate (plugin, sampleRate, 1u, (uint32_t) jmax (1, blockSize))) return;
    active = true;
    processing = plugin->start_processing (plugin);

    inPtrs.assign ((size_t) jmax (1, mainInChannels), nullptr);
    outPtrs.assign ((size_t) jmax (1, mainOutChannels), nullptr);
    /* A plugin with an input port that JUCE does not give us one for still has
       to be handed something: silence, not a null pointer. */
    silentIn.setSize (jmax (1, mainInChannels), jmax (1, blockSize));
    silentIn.clear();

    /* Reserve the event scratch now so the audio thread never has to grow it.
       A block can carry at most one event per sample in practice; reserving
       for the block size plus every parameter covers both notes and a full
       parameter re-send. */
    const size_t evRoom = (size_t) jmax (256, blockSize) + (size_t) clapParams.size();
    events.list.reserve (evRoom);
    events.paramStore.reserve (evRoom);
    events.noteStore.reserve (evRoom);
    events.midiStore.reserve (evRoom);

    /* Reactivating resets the plugin to its own defaults, so anything the host
       had set is gone. Queue every current value to be re-sent at the first
       block -- otherwise preparing to play silently undoes a sound, which is
       exactly what happens when the audio device changes mid-session. */
    if (params != nullptr)
        for (auto* cp : clapParams)
            queueParameterChange (cp->getClapId(), cp->toPlain (cp->getValue()));
}

void ClapInstance::releaseResources()
{
    if (plugin == nullptr || ! active) return;
    if (processing) plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    active = processing = false;
}

void ClapInstance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    if (plugin == nullptr || ! processing || numSamples <= 0)
    {
        for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, numSamples);
        return;
    }

    /* Events in: parameter changes queued from the UI, then notes. CLAP wants
       them in ascending time order, and everything we send is at frame 0 or at
       the message's own sample position, so the queue goes first. */
    /* Reused across blocks and reserved in prepareToPlay: processBlock runs on
       the audio thread and must not allocate. Capacity only ever grows, and
       only if a block somehow carries more events than reserved. */
    auto& ev = events;
    ev.list.clear();
    ev.paramStore.clear();
    ev.noteStore.clear();
    ev.midiStore.clear();

    {
        auto r = queueRead.load (std::memory_order_relaxed);
        const auto w = queueWrite.load (std::memory_order_acquire);
        while (r != w)
        {
            const auto pc = queue[r];
            clap_event_param_value_t e {};
            e.header.size = sizeof (e);
            e.header.type = CLAP_EVENT_PARAM_VALUE;
            e.header.time = 0;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.param_id = pc.id;
            e.port_index = -1; e.key = -1; e.channel = -1; e.note_id = -1;
            e.value = pc.plain;
            ev.paramStore.push_back (e);
            r = (r + 1) % queueCapacity;
        }
        queueRead.store (r, std::memory_order_release);
    }

    if (hasNoteInput)
    {
        ev.noteStore.reserve ((size_t) midi.getNumEvents());
        ev.midiStore.reserve ((size_t) midi.getNumEvents());
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            const auto time = (uint32_t) jlimit (0, numSamples - 1, meta.samplePosition);

            if (! noteDialectMidi && (m.isNoteOn() || m.isNoteOff()))
            {
                clap_event_note_t n {};
                n.header.size = sizeof (n);
                n.header.type = m.isNoteOn() ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
                n.header.time = time;
                n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                n.note_id = -1;
                n.port_index = 0;
                n.channel = (int16_t) (m.getChannel() - 1);
                n.key = (int16_t) m.getNoteNumber();
                n.velocity = m.isNoteOn() ? m.getFloatVelocity() : 0.0;
                ev.noteStore.push_back (n);
            }
            else
            {
                // Everything else -- CC, bend, pressure -- goes as MIDI 1.0.
                const auto* raw = m.getRawData();
                const int len = m.getRawDataSize();
                if (len < 1 || len > 3) continue;
                clap_event_midi_t e {};
                e.header.size = sizeof (e);
                e.header.type = CLAP_EVENT_MIDI;
                e.header.time = time;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.port_index = 0;
                std::memset (e.data, 0, sizeof (e.data));
                std::memcpy (e.data, raw, (size_t) len);
                ev.midiStore.push_back (e);
            }
        }
    }

    /* The pointers are gathered after the stores are filled: a vector that
       reallocates mid-fill would leave the list pointing at freed memory. */
    ev.list.reserve (ev.paramStore.size() + ev.noteStore.size() + ev.midiStore.size());
    for (auto& e : ev.paramStore) ev.list.push_back (&e.header);
    for (auto& e : ev.noteStore)  ev.list.push_back (&e.header);
    for (auto& e : ev.midiStore)  ev.list.push_back (&e.header);
    std::stable_sort (ev.list.begin(), ev.list.end(),
                      [] (auto* a, auto* b) { return a->time < b->time; });

    clap_input_events_t in {};
    in.ctx = &ev;
    in.size = [] (const clap_input_events_t* l) -> uint32_t
    { return (uint32_t) static_cast<Events*> (l->ctx)->list.size(); };
    in.get = [] (const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t*
    { return static_cast<Events*> (l->ctx)->list[(size_t) i]; };

    /* Output events: the plugin tells us when it changes a parameter itself,
       which is what makes a knob on the plugin's own GUI move the mapping. */
    clap_output_events_t out {};
    out.ctx = this;
    out.try_push = [] (const clap_output_events_t* l, const clap_event_header_t* h) -> bool
    {
        auto* me = static_cast<ClapInstance*> (l->ctx);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* pv = reinterpret_cast<const clap_event_param_value_t*> (h);
            for (auto* cp : me->clapParams)
                if (cp->getClapId() == pv->param_id) { cp->setFromPlugin (pv->value); break; }
        }
        return true;
    };

    // --- buffers ----------------------------------------------------------
    for (size_t ch = 0; ch < inPtrs.size(); ++ch)
        inPtrs[ch] = (int) ch < buffer.getNumChannels() && (int) ch < mainInChannels
                       ? buffer.getWritePointer ((int) ch)
                       : silentIn.getWritePointer ((int) jmin ((int) ch, silentIn.getNumChannels() - 1));
    for (size_t ch = 0; ch < outPtrs.size(); ++ch)
        outPtrs[ch] = (int) ch < buffer.getNumChannels()
                        ? buffer.getWritePointer ((int) ch)
                        : silentIn.getWritePointer (0);

    clap_audio_buffer_t ab_in {}, ab_out {};
    ab_in.data32 = inPtrs.data();   ab_in.channel_count  = (uint32_t) inPtrs.size();
    ab_out.data32 = outPtrs.data(); ab_out.channel_count = (uint32_t) outPtrs.size();

    clap_process_t proc {};
    proc.steady_time = steadyTime;
    proc.frames_count = (uint32_t) numSamples;
    proc.transport = nullptr;           // Performer has no transport; CLAP allows null
    proc.audio_inputs = mainInChannels > 0 ? &ab_in : nullptr;
    proc.audio_inputs_count = mainInChannels > 0 ? 1u : 0u;
    proc.audio_outputs = &ab_out;
    proc.audio_outputs_count = 1;
    proc.in_events = &in;
    proc.out_events = &out;

    const auto status = plugin->process (plugin, &proc);
    steadyTime += numSamples;

    if (status == CLAP_PROCESS_ERROR)
        buffer.clear();

    // Anything JUCE gave us beyond the plugin's own outputs must not be stale.
    for (int ch = mainOutChannels; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    midi.clear();                       // we do not produce MIDI
}

//==============================================================================
void ClapInstance::getStateInformation (MemoryBlock& destData)
{
    destData.reset();
    if (state == nullptr || state->save == nullptr) return;

    clap_ostream_t os {};
    os.ctx = &destData;
    os.write = [] (const clap_ostream_t* stream, const void* src, uint64_t numBytes) -> int64_t
    {
        static_cast<MemoryBlock*> (stream->ctx)->append (src, (size_t) numBytes);
        return (int64_t) numBytes;
    };
    state->save (plugin, &os);
}

void ClapInstance::setStateInformation (const void* data, int size)
{
    if (state == nullptr || state->load == nullptr || data == nullptr || size <= 0) return;

    struct Reader { const char* data; int size; int pos; } reader { (const char*) data, size, 0 };
    clap_istream_t is {};
    is.ctx = &reader;
    is.read = [] (const clap_istream_t* stream, void* dest, uint64_t numBytes) -> int64_t
    {
        auto* r = static_cast<Reader*> (stream->ctx);
        const auto n = jmin ((int) numBytes, r->size - r->pos);
        if (n <= 0) return 0;
        std::memcpy (dest, r->data + r->pos, (size_t) n);
        r->pos += n;
        return (int64_t) n;
    };
    state->load (plugin, &is);

    // The plugin's parameters have moved underneath us; tell the UI.
    if (params != nullptr && params->get_value != nullptr)
        for (auto* cp : clapParams)
        {
            double v = 0.0;
            if (params->get_value (plugin, cp->getClapId(), &v)) cp->setFromPlugin (v);
        }
}

//==============================================================================
/** The plugin's own window, embedded in a JUCE component.

    CLAP hands us a native X11 window id to parent into, which is exactly what
    JUCE's XEmbedComponent exists for. */
class ClapEditor final : public AudioProcessorEditor
{
public:
    ClapEditor (ClapInstance& o) : AudioProcessorEditor (o), owner (o)
    {
        auto* plugin = owner.getPlugin();
        auto* gui = owner.gui;

        if (gui != nullptr && gui->create != nullptr
            && gui->create (plugin, CLAP_WINDOW_API_X11, false))
        {
            created = true;

            uint32_t w = 600, h = 400;
            if (gui->get_size != nullptr) gui->get_size (plugin, &w, &h);
            setSize ((int) jmax (64u, w), (int) jmax (64u, h));

            /* Parent the plugin into our own window. addToDesktop gives this
               component a native peer whose handle CLAP can use. */
            addToDesktop (0);
            if (auto* peer = getPeer())
            {
                clap_window_t win {};
                win.api = CLAP_WINDOW_API_X11;
                win.x11 = (unsigned long) (pointer_sized_int) peer->getNativeHandle();
                if (gui->set_parent != nullptr && gui->set_parent (plugin, &win))
                    if (gui->show != nullptr) gui->show (plugin);
            }
        }
        else
        {
            setSize (400, 120);
        }
    }

    ~ClapEditor() override
    {
        if (created && owner.gui != nullptr && owner.gui->destroy != nullptr)
            owner.gui->destroy (owner.getPlugin());
    }

    void paint (Graphics& g) override
    {
        if (created) return;
        g.fillAll (Colours::black);
        g.setColour (Colours::white);
        g.drawFittedText ("This plugin has no window that can be embedded.",
                          getLocalBounds().reduced (10), Justification::centred, 2);
    }

private:
    ClapInstance& owner;
    bool created = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClapEditor)
};

AudioProcessorEditor* ClapInstance::createEditor()
{
    return gui != nullptr ? new ClapEditor (*this) : nullptr;
}

//==============================================================================
std::unique_ptr<AudioPluginInstance> ClapPluginFormat::createInstance (const PluginDescription& desc,
                                                                      double sampleRate, int blockSize,
                                                                      String& error)
{
    const File file (pathFromIdentifier (desc.fileOrIdentifier));
    const auto wantedId = pluginIdFromIdentifier (desc.fileOrIdentifier);

    auto bundle = Bundle::open (file);
    if (bundle == nullptr) { error = "Could not open " + file.getFileName(); return {}; }

    const auto* factory = bundle->factory();
    if (factory == nullptr || factory->create_plugin == nullptr)
    { error = "That file has no CLAP plugin factory."; return {}; }

    /* Find the descriptor whose id we were asked for; a bundle can hold
       several, and loading the wrong one would be worse than failing. */
    String id = wantedId;
    if (id.isEmpty())
    {
        const uint32_t n = factory->get_plugin_count (factory);
        if (n > 0)
            if (const auto* d = factory->get_plugin_descriptor (factory, 0))
                if (d->id != nullptr) id = String::fromUTF8 (d->id);
    }
    if (id.isEmpty()) { error = "That bundle holds no plugins."; return {}; }

    auto instance = std::make_unique<ClapInstance> (bundle, nullptr, desc);
    const auto* created = factory->create_plugin (factory, instance->hostStruct(), id.toRawUTF8());
    if (created == nullptr) { error = "The plugin could not be created."; return {}; }

    instance->adopt (created);
    if (! instance->initialise (sampleRate, blockSize, error)) return {};
    return instance;
}

void ClapPluginFormat::createPluginInstance (const PluginDescription& desc, double sampleRate,
                                             int blockSize, PluginCreationCallback callback)
{
    String error;
    auto instance = createInstance (desc, sampleRate, blockSize, error);
    if (callback != nullptr)
        callback (std::move (instance), error);
}

//==============================================================================
ClapPluginFormat::ClapPluginFormat() = default;
ClapPluginFormat::~ClapPluginFormat() = default;

String ClapPluginFormat::makeIdentifier (const String& path, const String& pluginId)
{
    return pluginId.isEmpty() ? path : path + String::charToString (kIdSeparator) + pluginId;
}

String ClapPluginFormat::pathFromIdentifier (const String& identifier)
{
    return identifier.upToFirstOccurrenceOf (String::charToString (kIdSeparator), false, false);
}

String ClapPluginFormat::pluginIdFromIdentifier (const String& identifier)
{
    return identifier.fromFirstOccurrenceOf (String::charToString (kIdSeparator), false, false);
}

//==============================================================================
void ClapPluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results,
                                            const String& fileOrIdentifier)
{
    const File file (pathFromIdentifier (fileOrIdentifier));
    if (! fileMightContainThisPluginType (file.getFullPathName())) return;

    auto bundle = Bundle::open (file);
    if (bundle == nullptr) return;

    const auto* factory = bundle->factory();
    if (factory == nullptr || factory->get_plugin_count == nullptr
        || factory->get_plugin_descriptor == nullptr)
        return;

    /* One bundle, possibly several plugins: Surge XT ships the synth and its
       effects in one file. Each gets its own description. */
    const auto wanted = pluginIdFromIdentifier (fileOrIdentifier);
    const uint32_t count = factory->get_plugin_count (factory);

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* d = factory->get_plugin_descriptor (factory, i);
        if (d == nullptr || d->id == nullptr) continue;

        // Asked for one in particular (a rescan of a known plugin): skip the rest.
        if (wanted.isNotEmpty() && String::fromUTF8 (d->id) != wanted) continue;

        results.add (new PluginDescription (describe (*d, file)));
    }
}

bool ClapPluginFormat::fileMightContainThisPluginType (const String& fileOrIdentifier)
{
    const File f (pathFromIdentifier (fileOrIdentifier));
    return f.hasFileExtension ("clap") && f.existsAsFile();
}

String ClapPluginFormat::getNameOfPluginFromIdentifier (const String& fileOrIdentifier)
{
    return File (pathFromIdentifier (fileOrIdentifier)).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning (const PluginDescription& desc)
{
    return File (pathFromIdentifier (desc.fileOrIdentifier)).getLastModificationTime()
             != desc.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist (const PluginDescription& desc)
{
    return File (pathFromIdentifier (desc.fileOrIdentifier)).existsAsFile();
}

StringArray ClapPluginFormat::searchPathsForPlugins (const FileSearchPath& path, bool recursive, bool)
{
    StringArray found;
    for (int i = 0; i < path.getNumPaths(); ++i)
        for (const auto& f : path[i].findChildFiles (File::findFiles, recursive, "*.clap"))
            found.add (f.getFullPathName());

    found.sort (true);
    return found;
}

FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    /* The locations the CLAP specification names for Linux, plus the one
       yabridge uses for bridged Windows plugins. */
    FileSearchPath path;
    path.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".clap"));
    path.add (File ("/usr/lib/clap"));
    path.add (File ("/usr/local/lib/clap"));
    return path;
}

} // namespace perf
