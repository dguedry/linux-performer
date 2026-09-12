/*
    performer-plugin-host: hosts exactly one plugin for Performer in a separate
    process.

        performer-plugin-host --serve <shm-name> <socket-fd>
        performer-plugin-host --scan  <format-name> <file-or-identifier>

    See Source/Ipc/Protocol.h for the wire protocol.
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../Ipc/Protocol.h"
#include "BinaryData.h"
#if JUCE_PLUGINHOST_VST3
 // Headers only: the IIDs are defined inside JUCE's own VST3 host translation unit.
 #define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
 #include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>
#endif
#include <iostream>
#include <mutex>
#include <map>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>

using namespace juce;
using namespace perf;

//==============================================================================
class PluginServer : private AudioProcessorListener
{
public:
    PluginServer (ipc::SharedBlock* block, int fd) : shm (block), socketFd (fd)
    {
        addDefaultFormatsToManager (formatManager);
        control = std::thread ([this] { controlLoop(); });
    }

    ~PluginServer()
    {
        stopAudioThread();
        // Bridged plugins (yabridge) throw if their other half is already gone;
        // nothing here may escape, we're on our way out anyway.
        try { editorWindow.reset(); } catch (...) {}
        if (instance != nullptr)
        {
            try { instance->removeListener (this); } catch (...) {}
            try { instance->releaseResources(); } catch (...) {}
            try { instance.reset(); } catch (...) {}
        }
        if (control.joinable()) control.join();
    }

    std::function<void()> onQuit;

private:
    //==============================================================================
    // Control channel: one thread reads requests and runs them on the message thread.
    void controlLoop()
    {
        ipc::FrameHeader header;
        MemoryBlock payload;
        while (ipc::readFrame (socketFd, header, payload))
        {
            struct Ctx { PluginServer* self; const ipc::FrameHeader* h; const MemoryBlock* p; MemoryBlock reply; };
            Ctx ctx { this, &header, &payload, {} };
            MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* c) -> void*
            {
                auto* x = static_cast<Ctx*> (c);
                try                          { x->reply = x->self->handleRequest ((ipc::Msg) x->h->type, *x->p); }
                catch (const std::exception& e) { x->reply = fail (String ("plugin threw: ") + e.what()); }
                catch (...)                  { x->reply = fail ("plugin threw an unknown exception"); }
                return nullptr;
            }, &ctx);

            {
                const std::lock_guard<std::mutex> l (sendMutex);
                ipc::sendFrame (socketFd, ipc::Msg::response, header.requestId, ctx.reply);
            }

            if ((ipc::Msg) header.type == ipc::Msg::quit)
                break;
        }
        // Host gone or told us to quit.
        MessageManager::callAsync ([this] { if (onQuit) onQuit(); });
    }

    static MemoryBlock ok (const MemoryOutputStream& data = MemoryOutputStream())
    {
        MemoryOutputStream out;
        out.writeBool (true);
        out.write (data.getData(), data.getDataSize());
        return out.getMemoryBlock();
    }

    static MemoryBlock fail (const String& why)
    {
        MemoryOutputStream out;
        out.writeBool (false);
        out.writeString (why);
        return out.getMemoryBlock();
    }

    MemoryBlock handleRequest (ipc::Msg type, const MemoryBlock& payload)
    {
        MemoryInputStream in (payload, false);
        switch (type)
        {
            case ipc::Msg::load:
            {
                const auto xmlText = in.readString();
                const double sr = in.readDouble();
                const int bs = in.readInt();
                PluginDescription desc;
                auto xml = parseXML (xmlText);
                if (xml == nullptr || ! desc.loadFromXml (*xml)) return fail ("bad plugin description");
                String error;
                auto inst = formatManager.createPluginInstance (desc, sr, bs, error);
                if (inst == nullptr) return fail (error.isEmpty() ? "could not create plugin" : error);

                inst->enableAllBuses();
                inst->setNonRealtime (false);
                inst->setPlayHead (nullptr);
                inst->prepareToPlay (sr, bs);
                inst->addListener (this);
                sampleRate = sr; blockSize = bs;
                {
                    const std::lock_guard<std::mutex> l (processMutex);
                    instance = std::move (inst);
                    const int ch = jmax (2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
                    scratch.setSize (ch, ipc::kMaxBlock);
                    midi.ensureSize (4096);
                }
                startAudioThread();
                return ok();
            }

            case ipc::Msg::prepare:
            {
                if (instance == nullptr) return fail ("no plugin");
                const double sr = in.readDouble();
                const int bs = in.readInt();
                const std::lock_guard<std::mutex> l (processMutex);
                instance->releaseResources();
                instance->prepareToPlay (sr, bs);
                sampleRate = sr; blockSize = bs;
                return ok();
            }

            case ipc::Msg::getInfo:
            {
                if (instance == nullptr) return fail ("no plugin");
                MemoryOutputStream out;
                out.writeString (instance->getName());
                out.writeBool (instance->acceptsMidi() && instance->getMainBusNumInputChannels() == 0);
                out.writeBool (instance->hasEditor());
                out.writeInt (instance->getLatencySamples());
                return ok (out);
            }

            case ipc::Msg::getParameters:
            {
                if (instance == nullptr) return fail ("no plugin");
                MemoryOutputStream out;
                auto& params = instance->getParameters();
                const auto midi = queryMidiAssignments (*instance);
                out.writeInt (params.size());
                for (auto* p : params)
                {
                    out.writeInt (p->getParameterIndex());
                    out.writeString (parameterId (*p));
                    out.writeString (p->getName (128));
                    out.writeBool (p->isAutomatable());
                    out.writeBool (p->isDiscrete());
                    out.writeBool (p->isBoolean());
                    out.writeFloat (p->getValue());
                    const auto& a = midi[(size_t) p->getParameterIndex()];
                    out.writeInt (a.channel);
                    out.writeInt (a.controller);
                }
                return ok (out);
            }

            case ipc::Msg::getParameterValue:
            {
                auto* p = paramAt (in.readInt());
                if (p == nullptr) return fail ("bad parameter");
                MemoryOutputStream out;
                out.writeFloat (p->getValue());
                return ok (out);
            }

            case ipc::Msg::setParameterValue:
            {
                auto* p = paramAt (in.readInt());
                const float v = in.readFloat();
                if (p == nullptr) return fail ("bad parameter");
                suppressNotifications = true;
                p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v));
                suppressNotifications = false;
                return ok();
            }

            case ipc::Msg::getState:
            {
                if (instance == nullptr) return fail ("no plugin");
                MemoryBlock state;
                instance->getStateInformation (state);
                MemoryOutputStream out;
                out.write (state.getData(), state.getSize());
                return ok (out);
            }

            case ipc::Msg::setState:
            {
                if (instance == nullptr) return fail ("no plugin");
                suppressNotifications = true;
                {
                    const std::lock_guard<std::mutex> l (processMutex);
                    instance->setStateInformation (payload.getData(), (int) payload.getSize());
                }
                suppressNotifications = false;
                return ok();
            }

            case ipc::Msg::showEditor:
            {
                if (instance == nullptr) return fail ("no plugin");
                const auto title = in.readString();
                if (editorWindow != nullptr) { editorWindow->toFront (true); return ok(); }
                editorWindow = std::make_unique<EditorWindow> (*this, title);
                {
                    MemoryOutputStream log;
                    log.writeString ("editor window " + editorWindow->getBounds().toString()
                                     + " visible=" + String ((int) editorWindow->isVisible())
                                     + " onDesktop=" + String ((int) editorWindow->isOnDesktop())
                                     + " peer=" + String ((int) (editorWindow->getPeer() != nullptr))
                                     + " messageThread=" + String ((int) MessageManager::getInstance()->isThisTheMessageThread()));
                    notify (ipc::Msg::notifyLog, log);
                }
                return ok();
            }

            case ipc::Msg::hideEditor:
                editorWindow.reset();
                return ok();

            case ipc::Msg::quit:
                stopAudioThread();
                return ok();

            default:
                return fail ("unknown request");
        }
    }

    AudioProcessorParameter* paramAt (int index) const
    {
        if (instance == nullptr) return nullptr;
        auto& params = instance->getParameters();
        return (index >= 0 && index < params.size()) ? params[index] : nullptr;
    }

    static String parameterId (const AudioProcessorParameter& p)
    {
        if (auto* hosted = dynamic_cast<const HostedAudioProcessorParameter*> (&p))
            return hosted->getParameterID();
        return String (p.getParameterIndex());
    }

    /** Which (MIDI channel, controller) a parameter stands for, or 0 / -1. */
    struct MidiAssignment { int channel = 0; int controller = -1; };

    /** VST3 plugins receive no MIDI controllers; they publish one parameter per
        (channel, controller) and the host converts, which is why Kontakt shows
        16 copies of "Channel Volume(MSB)". IMidiMapping tells us which copy is
        which channel, so the UI can label them. Empty assignments when the
        plugin does not expose the interface (LV2, LADSPA, some native VST3s
        whose controller is a separate object). */
    static std::vector<MidiAssignment> queryMidiAssignments (AudioPluginInstance& inst)
    {
        std::vector<MidiAssignment> result ((size_t) inst.getParameters().size());
       #if JUCE_PLUGINHOST_VST3
        auto* client = inst.getVST3Client();
        auto* component = client != nullptr ? client->getIComponentPtr() : nullptr;
        if (component == nullptr) return result;

        Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping> mapping (component);
        if (mapping == nullptr)
        {
            // Single-object plugins answer for the controller through the component.
            Steinberg::FUnknownPtr<Steinberg::Vst::IEditController> controller (component);
            if (controller != nullptr)
                mapping = Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping> (controller.get());
        }
        if (mapping == nullptr)
        {
            std::fprintf (stderr, "[params] plugin exposes no IMidiMapping through its component; controller copies stay unlabelled\n");
            return result;
        }

        std::map<String, int> indexById;
        for (auto* p : inst.getParameters())
            indexById[parameterId (*p)] = p->getParameterIndex();

        int found = 0;
        for (int ch = 0; ch < 16; ++ch)
            for (int cc = 0; cc <= (int) Steinberg::Vst::kCtrlProgramChange; ++cc)
            {
                Steinberg::Vst::ParamID id = 0;
                if (mapping->getMidiControllerAssignment (0, (Steinberg::int16) ch, (Steinberg::Vst::CtrlNumber) cc, id) != Steinberg::kResultTrue)
                    continue;
                const auto it = indexById.find (String ((uint32) id));
                if (it == indexById.end() || it->second < 0 || it->second >= (int) result.size()) continue;
                auto& a = result[(size_t) it->second];
                if (a.channel != 0) continue;       // first assignment wins
                a.channel = ch + 1;
                a.controller = cc;
                ++found;
            }
        std::fprintf (stderr, "[params] %d parameters are MIDI controller proxies\n", found);
       #endif
        return result;
    }

    void notify (ipc::Msg type, const MemoryOutputStream& data)
    {
        const std::lock_guard<std::mutex> l (sendMutex);
        ipc::sendFrame (socketFd, type, 0, data.getMemoryBlock());
    }

    // AudioProcessorListener --------------------------------------------------------
    void audioProcessorParameterChanged (AudioProcessor*, int index, float value) override
    {
        // GUI edits arrive on the message thread; anything else (automation, our own
        // block-driven changes) is not a "touch" and must not block the audio thread.
        if (suppressNotifications || ! MessageManager::getInstance()->isThisTheMessageThread()) return;
        MemoryOutputStream out; out.writeInt (index); out.writeFloat (value);
        notify (ipc::Msg::notifyParamChanged, out);
    }
    void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int index) override
    {
        if (! MessageManager::getInstance()->isThisTheMessageThread()) return;
        auto* p = paramAt (index);
        MemoryOutputStream out; out.writeInt (index); out.writeFloat (p != nullptr ? p->getValue() : 0.0f);
        notify (ipc::Msg::notifyParamTouched, out);
    }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override {}

    //==============================================================================
    // Editor window
    struct EditorWindow : public DocumentWindow
    {
        EditorWindow (PluginServer& s, const String& title)
            : DocumentWindow (title, Colours::darkgrey, DocumentWindow::closeButton), server (s)
        {
            setUsingNativeTitleBar (true);
            AudioProcessorEditor* editor = server.instance->hasEditor() ? server.instance->createEditorIfNeeded() : nullptr;
            if (editor != nullptr && (editor->getWidth() < 10 || editor->getHeight() < 10)) { delete editor; editor = nullptr; }
            if (editor == nullptr)
            {
                auto* generic = new GenericAudioProcessorEditor (*server.instance);
                generic->setSize (jmax (400, generic->getWidth()), jmax (300, generic->getHeight()));
                editor = generic;
            }
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            setResizeLimits (200, 100, 8192, 8192);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            if (auto* peer = getPeer()) peer->setIcon (ImageCache::getFromMemory (BinaryData::performer256_png, BinaryData::performer256_pngSize));
        }

        void closeButtonPressed() override
        {
            server.notify (ipc::Msg::notifyEditorClosed, MemoryOutputStream());
            MessageManager::callAsync ([&s = server] { s.editorWindow.reset(); });
        }

        bool keyPressed (const KeyPress& key) override
        {
            if (key == KeyPress::escapeKey) { closeButtonPressed(); return true; }
            return DocumentWindow::keyPressed (key);
        }

        PluginServer& server;
    };

    //==============================================================================
    // Audio thread: waits for blocks from the host and renders them.
    struct AudioThread : public Thread
    {
        explicit AudioThread (PluginServer& s) : Thread ("plugin-rt"), server (s) {}
        void run() override { server.audioLoop (*this); }
        PluginServer& server;
    };

    void startAudioThread()
    {
        if (audioThread != nullptr) return;
        audioThread = std::make_unique<AudioThread> (*this);
        if (! audioThread->startRealtimeThread (Thread::RealtimeOptions().withPriority (9)))
            audioThread->startThread (Thread::Priority::highest);
    }

    void stopAudioThread()
    {
        if (audioThread == nullptr) return;
        audioThread->signalThreadShouldExit();
        ::sem_post (&shm->request);
        audioThread->stopThread (2000);
        audioThread.reset();
    }

    void audioLoop (Thread& thread)
    {
        while (! thread.threadShouldExit())
        {
            if (! ipc::waitSemaphoreUntil (shm->request, ipc::monotonicDeadline (0.2)))
                continue;
            if (shm->quit.load() != 0 || thread.threadShouldExit())
                break;

            const auto seq = shm->requestSeq.load (std::memory_order_acquire);
            if (shm->completedSeq.load (std::memory_order_relaxed) == seq)
                continue;   // spurious wake

            try { renderBlock(); }
            catch (...)
            {
                // A throwing plugin produces silence for this block instead of killing the process.
                FloatVectorOperations::clear (shm->outL, ipc::kMaxBlock);
                FloatVectorOperations::clear (shm->outR, ipc::kMaxBlock);
            }

            shm->completedSeq.store (seq, std::memory_order_release);
            ::sem_post (&shm->done);
        }
    }

    void renderBlock()
    {
        const int n = jlimit (1, ipc::kMaxBlock, shm->numSamples);
        FloatVectorOperations::clear (shm->outL, n);
        FloatVectorOperations::clear (shm->outR, n);

        const std::lock_guard<std::mutex> l (processMutex);
        if (instance == nullptr || instance->isSuspended()) return;

        midi.clear();
        for (uint32_t i = 0; i < shm->midiCount && i < ipc::kMaxMidi; ++i)
        {
            const auto& ev = shm->midi[i];
            midi.addEvent (ev.data, (int) ev.size, ev.samplePosition);
        }

        if (shm->paramChangeCount > 0)
        {
            suppressNotifications = true;
            auto& params = instance->getParameters();
            for (uint32_t i = 0; i < shm->paramChangeCount && i < ipc::kMaxParamChanges; ++i)
            {
                const auto& pc = shm->paramChanges[i];
                if ((int) pc.index < params.size())
                    params[(int) pc.index]->setValueNotifyingHost (jlimit (0.0f, 1.0f, pc.value));
            }
            suppressNotifications = false;
        }

        AudioBuffer<float> view (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), n);
        view.clear();

        const int nIn = instance->getMainBusNumInputChannels();
        if (nIn >= 2)
        {
            view.copyFrom (0, 0, shm->inL, n);
            view.copyFrom (1, 0, shm->inR, n);
        }
        else if (nIn == 1)
        {
            view.copyFrom (0, 0, shm->inL, n);
            view.addFrom (0, 0, shm->inR, n);
            view.applyGain (0, 0, n, 0.5f);
        }

        instance->processBlock (view, midi);

        const int nOut = instance->getMainBusNumOutputChannels();
        if (nOut <= 0) return;
        FloatVectorOperations::copy (shm->outL, view.getReadPointer (0), n);
        FloatVectorOperations::copy (shm->outR, view.getReadPointer (nOut == 1 ? 0 : 1), n);
    }

    //==============================================================================
    ipc::SharedBlock* shm;
    int socketFd;
    AudioPluginFormatManager formatManager;
    std::unique_ptr<AudioPluginInstance> instance;
    std::unique_ptr<EditorWindow> editorWindow;
    std::unique_ptr<AudioThread> audioThread;
    std::thread control;
    std::mutex sendMutex, processMutex;
    std::atomic<bool> suppressNotifications { false };
    AudioBuffer<float> scratch;
    MidiBuffer midi;
    double sampleRate = 44100.0;
    int blockSize = 512;
};

//==============================================================================
/** Every process descended from `root` (not including root itself). */
static std::vector<pid_t> collectDescendants (pid_t root)
{
    std::vector<std::pair<pid_t, pid_t>> procs;   // (pid, ppid)
    if (DIR* d = ::opendir ("/proc"))
    {
        while (auto* e = ::readdir (d))
        {
            const pid_t pid = (pid_t) atoi (e->d_name);
            if (pid <= 0) continue;
            FILE* f = ::fopen (("/proc/" + std::string (e->d_name) + "/stat").c_str(), "r");
            if (f == nullptr) continue;
            char buf[512] = {};
            const auto n = ::fread (buf, 1, sizeof (buf) - 1, f);
            ::fclose (f);
            if (n == 0) continue;
            // "pid (comm) state ppid ..." -- comm may contain spaces, so find the last ')'
            const char* close = ::strrchr (buf, ')');
            if (close == nullptr) continue;
            int ppid = 0; char state = 0;
            if (::sscanf (close + 1, " %c %d", &state, &ppid) == 2)
                procs.emplace_back (pid, (pid_t) ppid);
        }
        ::closedir (d);
    }

    std::vector<pid_t> found { root };
    for (size_t i = 0; i < found.size(); ++i)
        for (auto& [pid, ppid] : procs)
            if (ppid == found[i] && std::find (found.begin(), found.end(), pid) == found.end())
                found.push_back (pid);
    found.erase (found.begin());
    return found;
}

/** Kills every process descended from us. Wine processes started by yabridge put
    themselves in their own session, so a process-group kill alone misses them. */
static void killDescendants (pid_t root)
{
    for (auto pid : collectDescendants (root))
        ::kill (pid, SIGKILL);
}

/** If Performer dies while our message thread is stuck inside a plugin call, the
    control thread never sees the connection close. Watch the parent directly. */
static void armParentWatchdog()
{
    const pid_t parent = ::getppid();
    std::thread ([parent]
    {
        for (;;)
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            if (::getppid() != parent)
            {
                std::cerr << "performer-plugin-host: Performer is gone, exiting\n";
                killDescendants (::getpid());
                ::kill (-::getpid(), SIGKILL);
            }
        }
    }).detach();
}

/** Plugin teardown can hang (yabridge waits for a Wine process that never exits).
    Once we've decided to exit, give cleanup a moment, then take our descendants and
    our whole process group down with us. */
static void armExitWatchdog (int graceMs)
{
    std::thread ([graceMs]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (graceMs));
        killDescendants (::getpid());
        ::kill (-::getpid(), SIGKILL);
    }).detach();
}

class PluginHostApplication : public JUCEApplication
{
public:
    const String getApplicationName() override       { return "performer-plugin-host"; }
    const String getApplicationVersion() override    { return "1.0"; }
    bool moreThanOneInstanceAllowed() override       { return true; }

    void initialise (const String& commandLine) override
    {
        // Own process group: lets the watchdog take stuck Wine children down with us,
        // without touching Performer.
        ::setpgid (0, 0);
        armParentWatchdog();

        ArgumentList args ("performer-plugin-host", commandLine);

        if (args.containsOption ("--scan"))
        {
            // --scan <format> <file-or-identifier>: print PluginDescription XML, one per
            // line, then an end marker. Our stdout must not leak into Wine processes the
            // plugin bridge spawns, or the parent would wait for them to exit.
            ::fcntl (STDOUT_FILENO, F_SETFD, FD_CLOEXEC);
            const int i = args.indexOfOption ("--scan");
            if (i < 0 || i + 2 >= args.size()) { setApplicationReturnValue (2); quit(); return; }
            const auto formatName = args[i + 1].text;
            const auto fileOrId = args[i + 2].text;

            // Bridged plugins sometimes hang in module unload, after their descriptions
            // have been collected, waiting for a Wine process that won't exit. If the
            // probe runs suspiciously long, kill our Wine descendants: the unload then
            // returns and the results still get printed.
            static std::atomic<bool> probeDone { false };
            std::thread ([]
            {
                std::this_thread::sleep_for (std::chrono::seconds (30));
                if (! probeDone.load())
                {
                    std::cerr << "performer-plugin-host: probe still running after 30 s, killing bridge processes\n";
                    killDescendants (::getpid());
                }
            }).detach();

            AudioPluginFormatManager fm;
            addDefaultFormatsToManager (fm);
            for (auto* f : fm.getFormats())
                if (f->getName() == formatName)
                {
                    // LV2 only resolves plugin classes (and thus isInstrument / category)
                    // once the whole world has been loaded, which the path search does.
                    if (formatName == "LV2")
                        f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, false);

                    OwnedArray<PluginDescription> found;
                    f->findAllTypesForFile (found, fileOrId);
                    for (auto* d : found)
                        if (auto xml = d->createXml())
                            std::cout << xml->toString (XmlElement::TextFormat().singleLine()).toRawUTF8() << "\n";
                }
            probeDone.store (true);
            std::cout << "<SCAN-DONE>\n";
            std::cout.flush();
            ::close (STDOUT_FILENO);      // the parent has everything; don't make it wait for teardown
            armExitWatchdog (3000);
            quit();
            return;
        }

        const int i = args.indexOfOption ("--serve");
        if (i < 0 || i + 2 >= args.size())
        {
            std::cerr << "usage: performer-plugin-host --serve <shm-name> <socket-fd> | --scan <format> <file>\n";
            setApplicationReturnValue (2);
            quit();
            return;
        }

        auto* shm = ipc::openSharedBlock (args[i + 1].text);
        const int fd = args[i + 2].text.getIntValue();
        if (fd >= 0) ::fcntl (fd, F_SETFD, FD_CLOEXEC);   // Wine children must not hold the control socket open
        if (shm == nullptr || fd < 0)
        {
            std::cerr << "performer-plugin-host: cannot open shared block\n";
            setApplicationReturnValue (1);
            quit();
            return;
        }

        server = std::make_unique<PluginServer> (shm, fd);
        server->onQuit = [this] { quit(); };
    }

    void shutdown() override
    {
        armExitWatchdog (3000);
        server.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<PluginServer> server;
};

START_JUCE_APPLICATION (PluginHostApplication)
