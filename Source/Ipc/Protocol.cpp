#include "Protocol.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>

namespace perf::ipc
{

bool writeAll (int fd, const void* data, size_t size)
{
    auto* p = static_cast<const uint8_t*> (data);
    while (size > 0)
    {
        const auto n = ::send (fd, p, size, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p += n; size -= (size_t) n;
    }
    return true;
}

bool readAll (int fd, void* data, size_t size)
{
    auto* p = static_cast<uint8_t*> (data);
    while (size > 0)
    {
        const auto n = ::recv (fd, p, size, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p += n; size -= (size_t) n;
    }
    return true;
}

bool sendFrame (int fd, Msg type, uint32_t requestId, const juce::MemoryBlock& payload)
{
    FrameHeader h { (uint32_t) payload.getSize(), (uint32_t) type, requestId };
    juce::MemoryBlock frame (sizeof (h) + payload.getSize());
    frame.copyFrom (&h, 0, sizeof (h));
    if (payload.getSize() > 0)
        frame.copyFrom (payload.getData(), (int) sizeof (h), payload.getSize());
    return writeAll (fd, frame.getData(), frame.getSize());
}

bool readFrame (int fd, FrameHeader& header, juce::MemoryBlock& payload)
{
    if (! readAll (fd, &header, sizeof (header))) return false;
    if (header.payloadSize > (256u << 20)) return false;     // 256 MB sanity limit
    payload.setSize (header.payloadSize);
    if (header.payloadSize > 0 && ! readAll (fd, payload.getData(), header.payloadSize)) return false;
    return true;
}

//==============================================================================
static SharedBlock* mapBlock (int fd)
{
    void* p = ::mmap (nullptr, sizeof (SharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close (fd);
    return p == MAP_FAILED ? nullptr : static_cast<SharedBlock*> (p);
}

SharedBlock* createSharedBlock (const juce::String& name)
{
    const int fd = ::shm_open (name.toRawUTF8(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return nullptr;
    if (::ftruncate (fd, (off_t) sizeof (SharedBlock)) != 0) { ::close (fd); ::shm_unlink (name.toRawUTF8()); return nullptr; }
    auto* b = mapBlock (fd);
    if (b == nullptr) { ::shm_unlink (name.toRawUTF8()); return nullptr; }

    std::memset (static_cast<void*> (b), 0, sizeof (SharedBlock));
    ::sem_init (&b->request, 1, 0);
    ::sem_init (&b->done, 1, 0);
    new (&b->requestSeq) std::atomic<uint32_t> (0);
    new (&b->completedSeq) std::atomic<uint32_t> (0);
    new (&b->quit) std::atomic<uint32_t> (0);
    b->magic = kMagic;
    b->version = kVersion;
    return b;
}

SharedBlock* openSharedBlock (const juce::String& name)
{
    const int fd = ::shm_open (name.toRawUTF8(), O_RDWR, 0600);
    if (fd < 0) return nullptr;
    auto* b = mapBlock (fd);
    if (b != nullptr && (b->magic != kMagic || b->version != kVersion)) { closeSharedBlock (b); return nullptr; }
    return b;
}

void closeSharedBlock (SharedBlock* b)
{
    if (b != nullptr) ::munmap (b, sizeof (SharedBlock));
}

void unlinkSharedBlock (const juce::String& name)
{
    ::shm_unlink (name.toRawUTF8());
}

//==============================================================================
timespec monotonicDeadline (double secondsFromNow)
{
    timespec ts {};
    ::clock_gettime (CLOCK_MONOTONIC, &ts);
    const auto ns = (long long) (secondsFromNow * 1e9);
    ts.tv_nsec += ns % 1000000000LL;
    ts.tv_sec  += (time_t) (ns / 1000000000LL);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec += 1; }
    return ts;
}

bool waitSemaphoreUntil (sem_t& sem, const timespec& deadline)
{
    for (;;)
    {
        if (::sem_clockwait (&sem, CLOCK_MONOTONIC, &deadline) == 0) return true;
        if (errno == EINTR) continue;
        return false;   // ETIMEDOUT
    }
}

} // namespace perf::ipc
