#include "PhoneTemplate.h"
#include "MappingSuggestions.h"      // findParamIndex

using namespace juce;

namespace perf
{

namespace
{
    constexpr const char* kFormat = "performer-phone-template";
    constexpr int kVersion = 1;

    String propStr (const var& v, const char* name)
    {
        if (auto* o = v.getDynamicObject()) return o->getProperty (name).toString();
        return {};
    }
}

//==============================================================================
var PhoneControl::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("id", paramId);
    if (paramName.isNotEmpty()) o->setProperty ("name", paramName);
    if (label.isNotEmpty())     o->setProperty ("label", label);
    if (widget != Widget::automatic)
        o->setProperty ("widget", widget == Widget::sw ? "switch" : "fader");
    return var (o);
}

PhoneControl PhoneControl::fromVar (const var& v)
{
    PhoneControl c;
    c.paramId   = propStr (v, "id");
    c.paramName = propStr (v, "name");
    c.label     = propStr (v, "label");

    const auto w = propStr (v, "widget");
    if (w == "switch")     c.widget = Widget::sw;
    else if (w == "fader") c.widget = Widget::fader;
    return c;
}

//==============================================================================
var PhoneTemplate::toVar() const
{
    auto* o = new DynamicObject();
    o->setProperty ("format", kFormat);
    o->setProperty ("version", kVersion);
    o->setProperty ("name", name);
    o->setProperty ("pluginName", pluginName);
    o->setProperty ("pluginKey", pluginKey);
    if (author.isNotEmpty()) o->setProperty ("author", author);
    if (notes.isNotEmpty())  o->setProperty ("notes", notes);

    Array<var> arr;
    for (const auto& c : controls) arr.add (c.toVar());
    o->setProperty ("controls", arr);
    return var (o);
}

PhoneTemplate PhoneTemplate::fromVar (const var& v)
{
    PhoneTemplate t;
    t.name       = propStr (v, "name");
    t.pluginName = propStr (v, "pluginName");
    t.pluginKey  = propStr (v, "pluginKey");
    t.author     = propStr (v, "author");
    t.notes      = propStr (v, "notes");

    if (auto* o = v.getDynamicObject())
        if (auto* arr = o->getProperty ("controls").getArray())
            for (const auto& c : *arr)
                if (auto ctl = PhoneControl::fromVar (c); ctl.paramId.isNotEmpty())
                    t.controls.push_back (ctl);
    return t;
}

//==============================================================================
String PhoneTemplate::Fit::summary() const
{
    const int total = matched + renamed + missing;
    if (total == 0) return "This template has no controls.";

    String s;
    s << matched + renamed << " of " << total << " controls found";
    if (renamed > 0)
        s << ", " << renamed << " under a different name";
    if (missing > 0)
        s << ", " << missing << " missing";
    return s + ".";
}

PhoneTemplate::Fit PhoneTemplate::checkAgainst (const ParamInfoList& params) const
{
    Fit fit;
    for (const auto& c : controls)
    {
        const int idx = findParamIndex (params, c.paramId);
        if (idx < 0) { ++fit.missing; continue; }

        const ParamInfo* info = nullptr;
        for (const auto& p : params) if (p.index == idx) info = &p;

        /* The name the plugin gives now, against the name it gave when the
           template was made. A mismatch does not stop anything -- a plugin
           update may simply have reworded it -- but it is exactly what a
           template applied to the wrong library looks like. */
        if (info != nullptr && c.paramName.isNotEmpty() && info->name != c.paramName)
            ++fit.renamed;
        else
            ++fit.matched;
    }
    return fit;
}

//==============================================================================
PhoneTemplates::PhoneTemplates (const File& storage) : file (storage)
{
    load();
}

std::vector<PhoneTemplate> PhoneTemplates::all() const { return templates; }

std::vector<PhoneTemplate> PhoneTemplates::forPlugin (const PluginDescription& d) const
{
    const auto key = d.createIdentifierString();
    std::vector<PhoneTemplate> out;
    for (const auto& t : templates)
        if (t.pluginKey == key)
            out.push_back (t);
    return out;
}

bool PhoneTemplates::contains (const String& name) const
{
    for (const auto& t : templates) if (t.name == name) return true;
    return false;
}

void PhoneTemplates::put (const PhoneTemplate& t)
{
    if (t.name.isEmpty()) return;
    for (auto& existing : templates)
        if (existing.name == t.name) { existing = t; save(); return; }
    templates.push_back (t);
    save();
}

void PhoneTemplates::remove (const String& name)
{
    const auto before = templates.size();
    templates.erase (std::remove_if (templates.begin(), templates.end(),
                                     [&] (const PhoneTemplate& t) { return t.name == name; }),
                     templates.end());
    if (templates.size() != before) save();
}

//==============================================================================
Result PhoneTemplates::save() const
{
    auto* root = new DynamicObject();
    root->setProperty ("format", "performer-phone-templates");
    root->setProperty ("version", kVersion);

    Array<var> arr;
    for (const auto& t : templates) arr.add (t.toVar());
    root->setProperty ("templates", arr);

    if (! file.getParentDirectory().createDirectory())
        return Result::fail ("Could not create " + file.getParentDirectory().getFullPathName());

    return file.replaceWithText (JSON::toString (var (root), false))
             ? Result::ok()
             : Result::fail ("Could not write " + file.getFullPathName());
}

Result PhoneTemplates::load()
{
    templates.clear();
    if (! file.existsAsFile()) return Result::ok();     // none yet is not an error

    var parsed;
    if (const auto r = JSON::parse (file.loadFileAsString(), parsed); r.failed()) return r;

    if (auto* o = parsed.getDynamicObject())
        if (auto* arr = o->getProperty ("templates").getArray())
            for (const auto& v : *arr)
                if (auto t = PhoneTemplate::fromVar (v); t.name.isNotEmpty())
                    templates.push_back (t);
    return Result::ok();
}

//==============================================================================
Result PhoneTemplates::exportToFile (const PhoneTemplate& t, const File& to)
{
    if (t.name.isEmpty()) return Result::fail ("The template needs a name.");
    return to.replaceWithText (JSON::toString (t.toVar(), false))
             ? Result::ok()
             : Result::fail ("Could not write " + to.getFullPathName());
}

Result PhoneTemplates::importFromFile (const File& from, PhoneTemplate& out)
{
    if (! from.existsAsFile()) return Result::fail (from.getFullPathName() + " does not exist.");

    var parsed;
    if (const auto r = JSON::parse (from.loadFileAsString(), parsed); r.failed())
        return Result::fail ("That is not a template file: " + r.getErrorMessage());

    /* Check the marker before anything else. Someone will eventually open a
       setup file here by mistake, and "no controls" is a worse message than
       "this is not a template". */
    if (propStr (parsed, "format") != kFormat)
        return Result::fail ("That file is not a Performer phone template.");

    if (auto* o = parsed.getDynamicObject(); o != nullptr && (int) o->getProperty ("version") > kVersion)
        return Result::fail ("That template was made by a newer version of Performer.");

    out = PhoneTemplate::fromVar (parsed);
    if (out.name.isEmpty())    return Result::fail ("That template has no name.");
    if (out.controls.empty())  return Result::fail ("That template has no controls in it.");
    return Result::ok();
}

} // namespace perf
