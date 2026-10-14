#include "frame_channel.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>

namespace {
    constexpr uint32_t kMagic = 0x4c535652;  // "LSVR"
    constexpr uint32_t kVersion = 5;
    constexpr size_t kSlotBytes =
        static_cast<size_t>(FrameChannel::kMaxWidth) * FrameChannel::kMaxHeight * 4;
    constexpr size_t kHeaderBytes = 4096;

    // The game's render thread takes this lock every frame, so it never waits
    // long: a hung or suspended host must not freeze the game. On timeout the
    // operation is skipped for that frame.
    constexpr DWORD kLockTimeoutMs = 5;

    class MutexLock {
    public:
        explicit MutexLock(void *mutex) : mutex_(mutex) {
            const DWORD result = WaitForSingleObject(mutex_, kLockTimeoutMs);
            // WAIT_ABANDONED (the other process died holding it) still grants ownership.
            owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
        ~MutexLock() {
            if (owned_) {
                ReleaseMutex(mutex_);
            }
        }
        bool owned() const {
            return owned_;
        }

    private:
        void *mutex_;
        bool owned_ = false;
    };
} // namespace

struct FrameChannel::Header {
    uint32_t magic;
    uint32_t version;
    volatile LONG host_wants_frames;
    int32_t published;  // slot index or -1
    int32_t reading;    // slot index or -1
    uint32_t pad;
    uint64_t next_sequence;
    struct {
        uint32_t width;
        uint32_t height;
        uint64_t sequence;
        VrFrameMeta meta;
    } slots[3];
    // Tracking state, host -> game. A seqlock: odd while the host is writing.
    volatile LONG head_seq;
    int32_t head_valid;
    VrTrackingState tracking;
};

bool FrameChannel::Map(uint32_t game_pid, bool create) {
    // Shared by the 32-bit game and the 64-bit host: the layout must match.
    static_assert(offsetof(Header, slots) == 32 && sizeof(Header::slots) == 3 * 64 && offsetof(Header, head_seq) == 224 &&
                      offsetof(Header, tracking) == 232,
                  "FrameChannel header layout");
    char name[64];
    sprintf_s(name, "Local\\lsw_vr_frames_%u", game_pid);
    const size_t total = kHeaderBytes + 3 * kSlotBytes;
    if (create) {
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(static_cast<uint64_t>(total) >> 32),
                                      static_cast<DWORD>(total), name);
    } else {
        mapping_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    }
    if (mapping_ == nullptr) {
        return false;
    }
    header_ = static_cast<Header *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (header_ == nullptr) {
        return false;
    }
    sprintf_s(name, "Local\\lsw_vr_frames_mutex_%u", game_pid);
    mutex_ = create ? CreateMutexA(nullptr, FALSE, name) : OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    if (mutex_ == nullptr) {
        return false;
    }
    if (create) {
        // Nobody else has the channel yet, so no lock is needed.
        header_->magic = kMagic;
        header_->version = kVersion;
        header_->host_wants_frames = 0;
        header_->published = -1;
        header_->reading = -1;
        header_->next_sequence = 1;
        header_->head_seq = 0;
        header_->head_valid = 0;
    }
    return header_->magic == kMagic && header_->version == kVersion;
}

bool FrameChannel::Create(uint32_t game_pid) {
    return Map(game_pid, true);
}

bool FrameChannel::Open(uint32_t game_pid) {
    return Map(game_pid, false);
}

FrameChannel::~FrameChannel() {
    if (header_ != nullptr) UnmapViewOfFile(header_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
    if (mutex_ != nullptr) CloseHandle(mutex_);
}

uint8_t *FrameChannel::Slot(int index) const {
    return reinterpret_cast<uint8_t *>(header_) + kHeaderBytes + index * kSlotBytes;
}

uint8_t *FrameChannel::BeginWrite(uint32_t width, uint32_t height) {
    if (header_ == nullptr || width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight) {
        return nullptr;
    }
    MutexLock lock(mutex_);
    if (!lock.owned()) {
        return nullptr;
    }
    for (int i = 0; i < 3; ++i) {
        if (i != header_->published && i != header_->reading) {
            write_slot_ = i;
            header_->slots[i].width = width;
            header_->slots[i].height = height;
            return Slot(i);
        }
    }
    return nullptr;  // unreachable with three slots
}

void FrameChannel::EndWrite(const VrFrameMeta &meta) {
    if (write_slot_ < 0) {
        return;
    }
    MutexLock lock(mutex_);
    if (!lock.owned()) {
        write_slot_ = -1;  // drop this frame; the slot stays free
        return;
    }
    header_->slots[write_slot_].meta = meta;
    header_->slots[write_slot_].sequence = header_->next_sequence++;
    header_->published = write_slot_;
    write_slot_ = -1;
}

const uint8_t *FrameChannel::BeginRead(uint64_t last_sequence, uint32_t *width, uint32_t *height,
                                       uint64_t *sequence, VrFrameMeta *meta) {
    if (header_ == nullptr) {
        return nullptr;
    }
    MutexLock lock(mutex_);
    if (!lock.owned()) {
        return nullptr;
    }
    const int slot = header_->published;
    if (slot < 0 || header_->slots[slot].sequence == last_sequence) {
        return nullptr;
    }
    header_->reading = slot;
    *width = header_->slots[slot].width;
    *height = header_->slots[slot].height;
    *sequence = header_->slots[slot].sequence;
    *meta = header_->slots[slot].meta;
    return Slot(slot);
}

void FrameChannel::EndRead() {
    // Must not be skipped (the slot would stay pinned), so wait as needed.
    WaitForSingleObject(mutex_, INFINITE);
    header_->reading = -1;
    ReleaseMutex(mutex_);
}

void FrameChannel::SetHostWantsFrames(bool wants) {
    if (header_ != nullptr) {
        InterlockedExchange(&header_->host_wants_frames, wants ? 1 : 0);
    }
}

bool FrameChannel::HostWantsFrames() const {
    return header_ != nullptr && header_->host_wants_frames != 0;
}

void FrameChannel::WriteTracking(const VrTrackingState &tracking) {
    if (header_ == nullptr) {
        return;
    }
    InterlockedIncrement(&header_->head_seq);  // odd: writing
    MemoryBarrier();
    header_->tracking = tracking;
    header_->head_valid = 1;
    MemoryBarrier();
    InterlockedIncrement(&header_->head_seq);  // even: stable
}

bool FrameChannel::ReadTracking(VrTrackingState *tracking) const {
    if (header_ == nullptr) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG before = header_->head_seq;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        const bool valid = header_->head_valid != 0;
        *tracking = header_->tracking;
        MemoryBarrier();
        if (header_->head_seq == before) {
            return valid;
        }
    }
    return false;
}

void FrameChannel::ClearTracking() {
    if (header_ == nullptr) {
        return;
    }
    InterlockedIncrement(&header_->head_seq);
    MemoryBarrier();
    header_->head_valid = 0;
    MemoryBarrier();
    InterlockedIncrement(&header_->head_seq);
}
