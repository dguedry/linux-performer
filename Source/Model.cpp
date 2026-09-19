#include "Model.h"

using namespace juce;

namespace perf
{

namespace
{
    DynamicObject* obj (const var& v) { return v.getDynamicObject(); }

    template <typename T>
    T prop (const var& v, const char* name, T def)
    {
        if (auto* o = obj (v))
            if (o->hasProperty (name))
                return static_cast<T> (o->getProperty (name));
        return def;
    }

    String propStr (const var& v, const char* name, const String& def = {})
    {
        if (auto* o = obj (v))
            if (o->hasProperty (name))
                return o->getProperty (name).toString();
        return def;
    }
}

//==============================================================================
static var effectsToVar (const std::vector<EffectDef>& effects)
{
    Array<var> arr;
    for (auto& e : effects) arr.add (e.toVar());
    return arr;
}

static std::vector<EffectDef> effectsFromVar (const var& parent)
{
    std::vector<EffectDef> out;
    if (auto* arr = obj (parent) != nullptr ? obj (parent)->getProperty ("effects").getArray() : nullptr)
        for (auto& e : *arr) out.push_back (EffectDef::fromVar (e));
    return out;
}

var EffectDef::toVar() const
{
    auto* o = new DynamicObject();
    if (auto xml = plugin.createXml())
        o->setProperty ("plugin", xml->toString (XmlElement::TextFormat().singleLine()));
    o->setProperty ("state", state.toBase64Encoding());
    o->setProperty ("bypassed", bypassed);
    return var (o);
}

EffectDef EffectDef::fromVar (const var& v)
{
    EffectDef e;
    if (auto xml = parseXML (propStr (v, "plugin")))
        e.plugin.loadFromXml (*xml);
    e.state.fromBase64Encoding (propStr (v, "state"));
    e.bypassed = prop<bool> (v, "bypassed", false);
    return e;
}

//==============================================================================
var SlotDef::toVar() const
{
    auto* o = new DynamicObject();
    if (auto xml = plugin.createXml())
        o->setProperty ("plugin", xml->toString (XmlElement::TextFormat().singleLine()));
    o->setProperty ("state", state.toBase64Encoding());
    o->setProperty ("enabled", enabled);
    o->setProperty ("gainDb", gainDb);
    o->setProperty ("transpose", transpose);
    o->setProperty ("lowKey", lowKey);
    o->setProperty ("highKey", highKey);
    o->setProperty ("lowVelocity", lowVelocity);
    o->setProperty ("highVelocity", highVelocity);
    o->setProperty ("velocityCurve", velocityCurve);
    o->setProperty ("pan", pan);
    o->setProperty ("outChannel", outChannel);
    o->setProperty ("effects", effectsToVar (effects));
    return var (o);
}

SlotDef SlotDef::fromVar (const var& v)
{
    SlotDef s;
    if (auto xml = parseXML (propStr (v, "plugin")))
        s.plugin.loadFromXml (*xml);
    s.state.fromBase64Encoding (propStr (v, "state"));
    s.enabled    = prop<bool>  (v, "enabled", true);
    s.gainDb     = prop<float> (v, "gainDb", 0.0f);
    s.transpose  = prop<int>   (v, "transpose", 0);
    s.lowKey     = prop<int>   (v, "lowKey", 0);
    s.highKey    = prop<int>   (v, "highKey", 127);
    s.lowVelocity   = juce::jlimit (1, 127, prop<int> (v, "lowVelocity", 1));
    s.highVelocity  = juce::jlimit (s.lowVelocity, 127, prop<int> (v, "highVelocity", 127));
    s.velocityCurve = juce::jlimit (-1.0f, 1.0f, prop<float> (v, "velocityCurve", 0.0f));
    s.pan           = juce::jlimit (-1.0f, 1.0f, prop<float> (v, "pan", 0.0f));
    s.outChannel = prop<int>   (v, "outChannel", 0);
    s.effects    = effectsFromVar (v);
    return s;
}

//==============================================================================
String MappingDef::sourceDescription() const
{
    switch (source)
    {
        case Source::CC:              return "CC " + String (number);
        case Source::PitchBend:       return "Pitch Bend";
        case Source::ChannelPressure: return "Aftertouch";
    }
    return {};
}

var MappingDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("source", (int) source);
    o->setProperty ("number", number);
    o->setProperty ("slot", slot);
    o->setProperty ("effect", effect);
    o->setProperty ("paramId", paramId);
    o->setProperty ("paramName", paramName);
    o->setProperty ("min", minValue);
    o->setProperty ("max", maxValue);
    o->setProperty ("passThrough", passThrough);
    return var (o);
}

MappingDef MappingDef::fromVar (const var& v)
{
    MappingDef m;
    m.source      = (Source) prop<int> (v, "source", 0);
    m.number      = prop<int>   (v, "number", 1);
    m.slot        = prop<int>   (v, "slot", 0);
    m.effect      = prop<int>   (v, "effect", -1);
    m.paramId     = propStr (v, "paramId");
    m.paramName   = propStr (v, "paramName");
    m.minValue    = prop<float> (v, "min", 0.0f);
    m.maxValue    = prop<float> (v, "max", 1.0f);
    m.passThrough = prop<bool>  (v, "passThrough", false);
    return m;
}

//==============================================================================
var ProgramDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    // Only written when set, so setups without groups stay byte-identical.
    if (group.isNotEmpty()) o->setProperty ("group", group);

    Array<var> slotArr;
    for (auto& s : slots) slotArr.add (s.toVar());
    o->setProperty ("slots", slotArr);
    o->setProperty ("effects", effectsToVar (effects));

    Array<var> mapArr;
    for (auto& m : mappings) mapArr.add (m.toVar());
    o->setProperty ("mappings", mapArr);
    return var (o);
}

ProgramDef ProgramDef::fromVar (const var& v)
{
    ProgramDef p;
    p.name = propStr (v, "name");
    p.group = propStr (v, "group");      // absent in setups written before groups
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("slots").getArray() : nullptr)
        for (auto& s : *arr) p.slots.push_back (SlotDef::fromVar (s));
    p.effects = effectsFromVar (v);
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("mappings").getArray() : nullptr)
        for (auto& m : *arr) p.mappings.push_back (MappingDef::fromVar (m));
    return p;
}

//==============================================================================
var InputDef::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("name", name);
    o->setProperty ("midiDeviceIdentifier", midiDeviceIdentifier);
    o->setProperty ("midiDeviceName", midiDeviceName);
    o->setProperty ("channel", channel);
    o->setProperty ("respondToProgramChange", respondToProgramChange);
    o->setProperty ("programChangeChannel", programChangeChannel);
    o->setProperty ("currentProgram", currentProgram);

    // Only store non-empty programs to keep files small.
    Array<var> progs;
    for (int i = 0; i < (int) programs.size(); ++i)
    {
        if (programs[(size_t) i].isEmpty() && programs[(size_t) i].name.isEmpty())
            continue;
        auto pv = programs[(size_t) i].toVar();
        pv.getDynamicObject()->setProperty ("index", i);
        progs.add (pv);
    }
    o->setProperty ("programs", progs);
    return var (o);
}

InputDef InputDef::fromVar (const var& v)
{
    InputDef in;
    in.name                   = propStr (v, "name", "Input");
    in.midiDeviceIdentifier   = propStr (v, "midiDeviceIdentifier");
    in.midiDeviceName         = propStr (v, "midiDeviceName");
    in.channel                = prop<int>  (v, "channel", 0);
    in.respondToProgramChange = prop<bool> (v, "respondToProgramChange", true);
    in.programChangeChannel   = juce::jlimit (-1, 16, prop<int> (v, "programChangeChannel", InputDef::pcChannelSameAsNotes));
    in.currentProgram         = jlimit (0, numPrograms - 1, prop<int> (v, "currentProgram", 0));

    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("programs").getArray() : nullptr)
        for (auto& pv : *arr)
        {
            int idx = prop<int> (pv, "index", -1);
            if (idx >= 0 && idx < numPrograms)
                in.programs[(size_t) idx] = ProgramDef::fromVar (pv);
        }
    return in;
}

//==============================================================================
var Setup::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("format", "performer-setup");
    o->setProperty ("version", 1);
    o->setProperty ("preloadAllPrograms", preloadAllPrograms);
    o->setProperty ("releaseTailSeconds", releaseTailSeconds);
    Array<var> arr;
    for (auto& i : inputs) arr.add (i.toVar());
    o->setProperty ("inputs", arr);
    return var (o);
}

Setup Setup::fromVar (const var& v)
{
    Setup s;
    s.preloadAllPrograms = prop<bool> (v, "preloadAllPrograms", false);
    s.releaseTailSeconds = prop<double> (v, "releaseTailSeconds", 4.0);
    if (auto* arr = obj (v) != nullptr ? obj (v)->getProperty ("inputs").getArray() : nullptr)
        for (auto& iv : *arr) s.inputs.push_back (InputDef::fromVar (iv));
    return s;
}

Result Setup::saveToFile (const File& f) const
{
    if (! f.getParentDirectory().createDirectory())
        return Result::fail ("Could not create directory " + f.getParentDirectory().getFullPathName());

    TemporaryFile temp (f);
    {
        FileOutputStream out (temp.getFile());
        if (! out.openedOk())
            return Result::fail ("Could not write " + f.getFullPathName());
        JSON::writeToStream (out, toVar());
    }
    if (! temp.overwriteTargetFileWithTemporary())
        return Result::fail ("Could not replace " + f.getFullPathName());
    return Result::ok();
}

Result Setup::loadFromFile (const File& f, Setup& out)
{
    if (! f.existsAsFile())
        return Result::fail ("File not found: " + f.getFullPathName());

    var parsed;
    auto r = JSON::parse (f.loadFileAsString(), parsed);
    if (r.failed())
        return r;
    if (propStr (parsed, "format") != "performer-setup")
        return Result::fail ("Not a Performer setup file");

    out = fromVar (parsed);
    return Result::ok();
}

Setup Setup::makeDefault()
{
    Setup s;
    InputDef upper; upper.name = "Upper"; upper.channel = 1;
    InputDef lower; lower.name = "Lower"; lower.channel = 2;
    s.inputs.push_back (upper);
    s.inputs.push_back (lower);
    return s;
}

} // namespace perf
