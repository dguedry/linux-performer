#include "RemotePlugin.h"
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <cstdio>

extern char** environ;

using namespace juce;

namespace perf
{

namespace
{
    constexpr int kLoadTimeoutMs     = 120000;
    constexpr int kStateTimeoutMs    = 30000;
    constexpr int kControlTimeoutMs  = 10000;
    constexpr int kEditorTimeoutMs   = 15000;
    constexpr int kQuitTimeoutMs     = 2000;

    std::atomic<int> shmCounter { 0 };

    void reapProcess (pid_t pid)
    {
        // Give it a moment to exit on its own, then insist.
        for (int i = 0; i < 60; ++i)
        {
            int status = 0;
            const auto r = ::waitpid (pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) return;
            Thread::sleep (50);
        }
        ::kill (pid, SIGKILL);
        int status = 0;
        ::waitpid (pid, &status, 0);
    }
}

//==============================================================================
RemotePlugin::RemotePlugin() {}

RemotePlugin::~RemotePlugin()
{
    shutdown();
}

File RemotePlugin::findHostExecutable()
{
    const auto env = SystemStats::getEnvironmentVariable ("PERFORMER_PLUGIN_HOST", {});
    if (env.isNotEmpty() && File (env).existsAsFile())
        return File (env);

    const auto exe = File::getSpecialLocation (File::currentExecutableFile);
    const auto dir = exe.getParentDirectory();
    const String name ("performer-plugin-host");

    for (auto& candidate : { dir.getChildFile (name),
                             dir.getParentDirectory().getParentDirectory().getChildFile ("PerformerPluginHost_artefacts").getChildFile (dir.getFileName()).getChildFile (name),
                             dir.getParentDirectory().getParentDirectory().getChildFile ("PerformerPluginHost_artefacts").getChildFile (name),
                             File ("/usr/local/bin").getChildFile (name),
                             File ("/usr/bin").getChildFile (name) })
        if (candidate.existsAsFile())
            return candidate;
    return {};
}

std::vector<std::string> RemotePlugin::buildHelperEnvironment (std::vector<char*>& pointers)
{
    std::vector<std::string> env;
    String path = SystemStats::getEnvironmentVariable ("PATH", "/usr/local/bin:/usr/bin:/bin");
    String wineLoader = SystemStats::getEnvironmentVariable ("WINELOADER", {});

    const auto home = File::getSpecialLocation (File::userHomeDirectory);
    const auto localBin = home.getChildFile (".local/bin");
    if (localBin.getChildFile ("wine").existsAsFile())
    {
        auto parts = StringArray::fromTokens (path, ":", {});
        parts.removeString (localBin.getFullPathName());
        parts.insert (0, localBin.getFullPathName());
        path = parts.joinIntoString (":");
    }

    // systemd environment.d files (KEY=value lines) are applied at login by desktops
    // that support it; a terminal or launcher that predates them misses out. Apply any
    // variable from there that this process doesn't already have -- that is where
    // nilinux puts WINELOADER and WINEFSYNC for DAWs.
    std::map<String, String> extra;
    for (const auto& conf : home.getChildFile (".config/environment.d").findChildFiles (File::findFiles, false, "*.conf"))
        for (auto& raw : StringArray::fromLines (conf.loadFileAsString()))
        {
            const auto line = raw.trim();
            if (line.isEmpty() || line.startsWith ("#") || ! line.contains ("=")) continue;
            const auto key = line.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto value = line.fromFirstOccurrenceOf ("=", false, false).trim().unquoted();
            if (key.isNotEmpty() && SystemStats::getEnvironmentVariable (key, {}).isEmpty())
                extra[key] = value;
        }
    if (wineLoader.isEmpty() && extra.count ("WINELOADER") != 0 && File (extra["WINELOADER"]).existsAsFile())
        wineLoader = extra["WINELOADER"];
    extra.erase ("WINELOADER");
    extra.erase ("PATH");

    for (char** e = environ; *e != nullptr; ++e)
    {
        const String entry (*e);
        if (entry.startsWith ("PATH=") || entry.startsWith ("WINELOADER=")) continue;
        env.push_back (*e);
    }
    env.push_back (("PATH=" + path).toStdString());
    if (wineLoader.isNotEmpty())
        env.push_back (("WINELOADER=" + wineLoader).toStdString());
    for (auto& [k, v] : extra)
        env.push_back ((k + "=" + v).toStdString());

    pointers.clear();
    for (auto& e : env) pointers.push_back (const_cast<char*> (e.c_str()));
    pointers.push_back (nullptr);
    return env;
}

String RemotePlugin::getLastError() const
{
    const std::lock_guard<std::mutex> l (errorMutex);
    return lastError;
}

//==============================================================================
bool RemotePlugin::spawn (String& error)
{
    const auto exe = findHostExecutable();
    if (exe == File())
    {
        error = "performer-plugin-host executable not found (set PERFORMER_PLUGIN_HOST)";
        return false;
    }

    shmName = "/performer-" + String ((int) ::getpid()) + "-" + String (shmCounter++) + "-" + String::toHexString (Random::getSystemRandom().nextInt64());
    shm = ipc::createSharedBlock (shmName);
    if (shm == nullptr) { error = "could not create shared memory " + shmName; return false; }

    int fds[2];
    if (::socketpair (AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    {
        error = "socketpair failed";
        return false;
    }

    // The child gets its socket end as fd 3; everything of ours stays private.
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init (&actions);
    ::posix_spawn_file_actions_adddup2 (&actions, fds[1], 3);
    if (fds[1] != 3) ::posix_spawn_file_actions_addclose (&actions, fds[1]);
    if (fds[0] != 3) ::posix_spawn_file_actions_addclose (&actions, fds[0]);

    const auto exePath = exe.getFullPathName().toStdString();
    const auto shmStd  = shmName.toStdString();
    const char* argv[] = { exePath.c_str(), "--serve", shmStd.c_str(), "3", nullptr };

    std::vector<char*> envp;
    const auto envStorage = buildHelperEnvironment (envp);
    const int rc = ::posix_spawn (&pid, exePath.c_str(), &actions, nullptr, const_cast<char**> (argv), envp.data());
    ::posix_spawn_file_actions_destroy (&actions);
    ::close (fds[1]);

    if (rc != 0)
    {
        ::close (fds[0]);
        pid = -1;
        error = "could not start " + exe.getFullPathName() + ": " + String (strerror (rc));
        return false;
    }

    ::fcntl (fds[0], F_SETFD, FD_CLOEXEC);
    socketFd = fds[0];
    alive.store (true);
    reader = std::thread ([this] { readerLoop(); });
    return true;
}

bool RemotePlugin::load (const PluginDescription& desc, double sampleRate, int blockSize, String& error)
{
    description = desc;
    name = desc.name;

    if (! spawn (error))
        return false;

    MemoryOutputStream out;
    if (auto xml = desc.createXml())
        out.writeString (xml->toString (XmlElement::TextFormat().singleLine()));
    else
        out.writeString ({});
    out.writeDouble (sampleRate);
    out.writeInt (blockSize);

    if (! request (ipc::Msg::load, out.getMemoryBlock(), kLoadTimeoutMs))
    {
        error = getLastError();
        if (error.isEmpty()) error = "plugin host did not respond";
        return false;
    }

    MemoryBlock info;
    if (request (ipc::Msg::getInfo, {}, kControlTimeoutMs, &info))
    {
        MemoryInputStream in (info, false);
        name = in.readString();
        instrument = in.readBool();
        editorAvailable = in.readBool();
    }

    MemoryBlock paramData;
    if (request (ipc::Msg::getParameters, {}, kControlTimeoutMs, &paramData))
    {
        MemoryInputStream in (paramData, false);
        const int count = in.readInt();
        params.clear();
        params.reserve ((size_t) jmax (0, count));
        for (int i = 0; i < count && ! in.isExhausted(); ++i)
        {
            ParamInfo p;
            p.index = in.readInt();
            p.id = in.readString();
            p.name = in.readString();
            p.automatable = in.readBool();
            p.discrete = in.readBool();
            p.boolean = in.readBool();
            p.value = in.readFloat();
            params.push_back (std::move (p));
        }
        paramValues.reset (new std::atomic<float>[params.size() + 1]);
        for (size_t i = 0; i < params.size(); ++i)
            paramValues[i].store (params[i].value);
    }
    return true;
}

bool RemotePlugin::prepare (double sampleRate, int blockSize)
{
    MemoryOutputStream out;
    out.writeDouble (sampleRate);
    out.writeInt (blockSize);
    return request (ipc::Msg::prepare, out.getMemoryBlock(), kControlTimeoutMs);
}

//==============================================================================
int RemotePlugin::findParameterIndex (const String& id) const
{
    for (auto& p : params)
        if (p.id == id) return p.index;
    if (id.containsOnly ("0123456789"))
    {
        const int idx = id.getIntValue();
        if (idx >= 0 && idx < (int) params.size()) return idx;
    }
    return -1;
}

float RemotePlugin::getCachedParameterValue (int index) const
{
    if (paramValues == nullptr || index < 0 || index >= (int) params.size()) return 0.0f;
    return paramValues[(size_t) index].load();
}

bool RemotePlugin::fetchParameterValue (int index, float& value)
{
    MemoryOutputStream out;
    out.writeInt (index);
    MemoryBlock resp;
    if (! request (ipc::Msg::getParameterValue, out.getMemoryBlock(), kControlTimeoutMs, &resp)) return false;
    MemoryInputStream in (resp, false);
    value = in.readFloat();
    if (paramValues != nullptr && index >= 0 && index < (int) params.size())
        paramValues[(size_t) index].store (value);
    return true;
}

bool RemotePlugin::setParameterValue (int index, float value)
{
    if (paramValues != nullptr && index >= 0 && index < (int) params.size())
        paramValues[(size_t) index].store (value);
    MemoryOutputStream out;
    out.writeInt (index);
    out.writeFloat (value);
    return request (ipc::Msg::setParameterValue, out.getMemoryBlock(), kControlTimeoutMs);
}

bool RemotePlugin::getState (MemoryBlock& state)
{
    return request (ipc::Msg::getState, {}, kStateTimeoutMs, &state);
}

bool RemotePlugin::setState (const MemoryBlock& state)
{
    return request (ipc::Msg::setState, state, kStateTimeoutMs);
}

bool RemotePlugin::showEditor (const String& title)
{
    MemoryOutputStream out;
    out.writeString (title);
    const bool ok = request (ipc::Msg::showEditor, out.getMemoryBlock(), kEditorTimeoutMs);
    if (ok) editorOpen.store (true);
    return ok;
}

bool RemotePlugin::hideEditor()
{
    editorOpen.store (false);
    return request (ipc::Msg::hideEditor, {}, kEditorTimeoutMs);
}

//==============================================================================
bool RemotePlugin::request (ipc::Msg type, const MemoryBlock& payload, int timeoutMs, MemoryBlock* response)
{
    if (! alive.load()) return false;

    Pending p;
    const auto id = nextRequestId++;
    {
        const std::lock_guard<std::mutex> l (pendingMutex);
        pending[id] = &p;
    }

    bool sent;
    {
        const std::lock_guard<std::mutex> l (sendMutex);
        sent = ipc::sendFrame (socketFd, type, id, payload);
    }

    const bool arrived = sent && p.event.wait (timeoutMs);
    {
        const std::lock_guard<std::mutex> l (pendingMutex);
        pending.erase (id);
    }

    if (! sent) { markDead ("connection to plugin host lost"); return false; }
    if (! arrived)
    {
        const std::lock_guard<std::mutex> l (errorMutex);
        lastError = "plugin host did not respond within " + String (timeoutMs / 1000) + " s";
        return false;
    }

    MemoryInputStream in (p.response, false);
    const bool ok = in.readBool();
    if (! ok)
    {
        const std::lock_guard<std::mutex> l (errorMutex);
        lastError = in.readString();
        return false;
    }
    if (response != nullptr)
    {
        const auto pos = (size_t) in.getPosition();
        response->setSize (p.response.getSize() - pos);
        if (response->getSize() > 0)
            response->copyFrom (static_cast<const char*> (p.response.getData()) + pos, 0, response->getSize());
    }
    return true;
}

void RemotePlugin::readerLoop()
{
    ipc::FrameHeader header;
    MemoryBlock payload;
    while (ipc::readFrame (socketFd, header, payload))
    {
        if ((ipc::Msg) header.type == ipc::Msg::response)
        {
            const std::lock_guard<std::mutex> l (pendingMutex);
            auto it = pending.find (header.requestId);
            if (it != pending.end())
            {
                it->second->response = payload;
                it->second->ok = true;
                it->second->event.signal();
            }
        }
        else
        {
            handleNotification (header, payload);
        }
    }
    markDead ("plugin host process ended");
}

void RemotePlugin::handleNotification (const ipc::FrameHeader& header, const MemoryBlock& payload)
{
    MemoryInputStream in (payload, false);
    switch ((ipc::Msg) header.type)
    {
        case ipc::Msg::notifyParamTouched:
        case ipc::Msg::notifyParamChanged:
        {
            const int index = in.readInt();
            const float value = in.readFloat();
            if (paramValues != nullptr && index >= 0 && index < (int) params.size())
                paramValues[(size_t) index].store (value);
            if (listener != nullptr)
            {
                if ((ipc::Msg) header.type == ipc::Msg::notifyParamTouched) listener->remoteParameterTouched (*this, index, value);
                else                                                        listener->remoteParameterChanged (*this, index, value);
            }
            break;
        }
        case ipc::Msg::notifyEditorClosed:
            editorOpen.store (false);
            if (listener != nullptr) listener->remoteEditorClosed (*this);
            break;
        case ipc::Msg::notifyLog:
            std::fprintf (stderr, "[plugin-host %s] %s\n", name.toRawUTF8(), in.readString().toRawUTF8());
            break;
        default:
            break;
    }
}

void RemotePlugin::markDead (const String& why)
{
    if (! alive.exchange (false)) return;
    {
        const std::lock_guard<std::mutex> l (errorMutex);
        lastError = why;
    }
    {
        const std::lock_guard<std::mutex> l (pendingMutex);
        for (auto& [id, p] : pending) p->event.signal();
    }
    if (listener != nullptr) listener->remoteDied (*this);
}

//==============================================================================
// Real-time
//==============================================================================
void RemotePlugin::queueParameterChange (int index, float value)
{
    if (paramValues != nullptr && index >= 0 && index < (int) params.size())
        paramValues[(size_t) index].store (value);
    if (pendingParamCount < ipc::kMaxParamChanges)
        pendingParams[pendingParamCount++] = { (uint32_t) index, value };
}

void RemotePlugin::drainDoneSemaphore()
{
    while (::sem_trywait (&shm->done) == 0) {}
}

void RemotePlugin::beginProcess (const float* inL, const float* inR, const MidiBuffer& midi, int numSamples)
{
    blockInFlight = false;
    if (shm == nullptr || ! alive.load()) return;

    // Still busy with a block it missed the deadline for? Skip this one.
    if (shm->completedSeq.load (std::memory_order_acquire) != shm->requestSeq.load (std::memory_order_relaxed))
        return;

    drainDoneSemaphore();

    const int n = jmin (numSamples, ipc::kMaxBlock);
    shm->numSamples = n;

    uint32_t count = 0;
    for (const auto meta : midi)
    {
        if (count >= ipc::kMaxMidi) break;
        if (meta.numBytes > 3 || meta.numBytes <= 0) continue;
        auto& ev = shm->midi[count++];
        ev.samplePosition = jlimit (0, n - 1, meta.samplePosition);
        ev.size = (uint8_t) meta.numBytes;
        std::memcpy (ev.data, meta.data, (size_t) meta.numBytes);
    }
    shm->midiCount = count;

    shm->paramChangeCount = pendingParamCount;
    if (pendingParamCount > 0)
        std::memcpy (shm->paramChanges, pendingParams, sizeof (ipc::ParamChange) * pendingParamCount);
    pendingParamCount = 0;

    if (inL != nullptr) FloatVectorOperations::copy (shm->inL, inL, n); else FloatVectorOperations::clear (shm->inL, n);
    if (inR != nullptr) FloatVectorOperations::copy (shm->inR, inR, n); else FloatVectorOperations::clear (shm->inR, n);

    shm->requestSeq.fetch_add (1, std::memory_order_release);
    ::sem_post (&shm->request);
    blockInFlight = true;
    inFlightSamples = n;
}

bool RemotePlugin::finishProcess (float* outL, float* outR, const timespec& deadline)
{
    if (! blockInFlight) return false;
    blockInFlight = false;

    const auto seq = shm->requestSeq.load (std::memory_order_relaxed);
    while (shm->completedSeq.load (std::memory_order_acquire) != seq)
    {
        if (! alive.load() || ! ipc::waitSemaphoreUntil (shm->done, deadline))
        {
            missedBlocks.fetch_add (1);
            return false;
        }
    }

    if (outL != nullptr) FloatVectorOperations::copy (outL, shm->outL, inFlightSamples);
    if (outR != nullptr) FloatVectorOperations::copy (outR, shm->outR, inFlightSamples);
    return true;
}

//==============================================================================
void RemotePlugin::shutdown()
{
    if (pid < 0 && socketFd < 0 && shm == nullptr) return;

    if (alive.load())
        request (ipc::Msg::quit, {}, kQuitTimeoutMs);

    if (shm != nullptr)
    {
        shm->quit.store (1);
        ::sem_post (&shm->request);
    }

    alive.store (false);
    if (socketFd >= 0)
    {
        ::shutdown (socketFd, SHUT_RDWR);
        if (reader.joinable()) reader.join();
        ::close (socketFd);
        socketFd = -1;
    }
    else if (reader.joinable())
    {
        reader.join();
    }

    if (pid > 0)
    {
        const pid_t p = pid;
        std::thread ([p] { reapProcess (p); }).detach();
        pid = -1;
    }

    if (shm != nullptr)
    {
        ipc::closeSharedBlock (shm);
        ipc::unlinkSharedBlock (shmName);
        shm = nullptr;
    }
}

} // namespace perf
