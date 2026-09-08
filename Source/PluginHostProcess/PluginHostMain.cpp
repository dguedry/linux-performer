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
#include <iostream>
#include <mutex>
#include <thread>

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
        editorWindow.reset();
        if (instance != nullptr)
        {
            instance->removeListener (this);
            instance->releaseResources();
            instance.reset();
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
                x->reply = x->self->handleRequest ((ipc::Msg) x->h->type, *x->p);
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

            renderBlock();

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
class PluginHostApplication : public JUCEApplication
{
public:
    const String getApplicationName() override       { return "performer-plugin-host"; }
    const String getApplicationVersion() override    { return "1.0"; }
    bool moreThanOneInstanceAllowed() override       { return true; }

    void initialise (const String& commandLine) override
    {
        ArgumentList args ("performer-plugin-host", commandLine);

        if (args.containsOption ("--scan"))
        {
            // --scan <format> <file-or-identifier>: print PluginDescription XML, one per line.
            const int i = args.indexOfOption ("--scan");
            if (i < 0 || i + 2 >= args.size()) { setApplicationReturnValue (2); quit(); return; }
            const auto formatName = args[i + 1].text;
            const auto fileOrId = args[i + 2].text;

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
            std::cout.flush();
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
        server.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<PluginServer> server;
};

START_JUCE_APPLICATION (PluginHostApplication)
