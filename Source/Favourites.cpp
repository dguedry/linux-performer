#include "Favourites.h"

using namespace juce;

namespace perf
{

Favourites::Favourites (const File& storage) : file (storage)
{
    load();
}

std::vector<String> Favourites::get (const PluginDescription& d) const
{
    const auto it = entries.find (keyFor (d));
    return it == entries.end() ? std::vector<String>() : it->second.paramIds;
}

void Favourites::set (const PluginDescription& d, const std::vector<String>& paramIds)
{
    if (paramIds.empty())
    {
        entries.erase (keyFor (d));
    }
    else
    {
        auto& e = entries[keyFor (d)];
        e.pluginName = d.name;
        e.paramIds = paramIds;
    }
    save();
}

void Favourites::add (const PluginDescription& d, const String& paramId)
{
    if (paramId.isEmpty() || contains (d, paramId)) return;
    auto ids = get (d);
    ids.push_back (paramId);
    set (d, ids);
}

void Favourites::remove (const PluginDescription& d, const String& paramId)
{
    auto ids = get (d);
    ids.erase (std::remove (ids.begin(), ids.end(), paramId), ids.end());
    set (d, ids);
}

bool Favourites::contains (const PluginDescription& d, const String& paramId) const
{
    const auto ids = get (d);
    return std::find (ids.begin(), ids.end(), paramId) != ids.end();
}

bool Favourites::hasAny (const PluginDescription& d) const
{
    return ! get (d).empty();
}

//==============================================================================
Result Favourites::save() const
{
    DynamicObject::Ptr root (new DynamicObject());
    for (const auto& [key, e] : entries)
    {
        DynamicObject::Ptr o (new DynamicObject());
        o->setProperty ("plugin", e.pluginName);
        Array<var> ids;
        for (const auto& id : e.paramIds) ids.add (id);
        o->setProperty ("params", ids);
        root->setProperty (key, var (o.get()));
    }

    if (! file.getParentDirectory().createDirectory())
        return Result::fail ("Could not create " + file.getParentDirectory().getFullPathName());

    return file.replaceWithText (JSON::toString (var (root.get()), false))
             ? Result::ok()
             : Result::fail ("Could not write " + file.getFullPathName());
}

Result Favourites::load()
{
    entries.clear();
    if (! file.existsAsFile()) return Result::ok();      // nothing chosen yet is not an error

    var parsed;
    const auto r = JSON::parse (file.loadFileAsString(), parsed);
    if (r.failed()) return r;

    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return Result::fail ("Favourites file is not an object");

    for (const auto& prop : obj->getProperties())
    {
        auto* o = prop.value.getDynamicObject();
        if (o == nullptr) continue;

        Entry e;
        e.pluginName = o->getProperty ("plugin").toString();
        if (auto* arr = o->getProperty ("params").getArray())
            for (const auto& id : *arr)
                if (id.toString().isNotEmpty())
                    e.paramIds.push_back (id.toString());

        if (! e.paramIds.empty())
            entries[prop.name.toString()] = std::move (e);
    }
    return Result::ok();
}

} // namespace perf
