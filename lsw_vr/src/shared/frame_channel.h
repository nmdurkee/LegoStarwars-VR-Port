#pragma once

#include <cstdint>

#include "vr_types.h"

// Cross-process frame hand-off between the mod (32-bit, in the game) and
// lsw_vr_host.exe (64-bit, runs OpenXR): a named shared-memory block with
// three frame slots, so the writer always has a free slot while the reader
// holds the newest, plus a named mutex guarding the slot bookkeeping (pixel
// copies happen outside it). Head and controller tracking flow the other
// way through a seqlock. Pixels are BGRA8, tightly packed rows. Compiled
// into both processes, so the layout uses only fixed-size fields.
class FrameChannel {
public:
    static constexpr uint32_t kMaxWidth = 2560;
    static constexpr uint32_t kMaxHeight = 1600;

    // Writer (game) creates the channel; the host opens it by game process id.
    bool Create(uint32_t game_pid);
    bool Open(uint32_t game_pid);
    ~FrameChannel();

    // Writer side. BeginWrite returns a buffer of width*height*4 bytes, or
    // nullptr if the size is too big; EndWrite publishes it with its metadata.
    uint8_t *BeginWrite(uint32_t width, uint32_t height);
    void EndWrite(const VrFrameMeta &meta);

    // Reader side: the newest frame if newer than last_sequence, else nullptr.
    const uint8_t *BeginRead(uint64_t last_sequence, uint32_t *width, uint32_t *height, uint64_t *sequence,
                             VrFrameMeta *meta);
    void EndRead();

    // Tracking (head and controllers): written by the host every headset
    // frame, read by the game. ReadTracking returns false until the host has a
    // tracked head pose.
    void WriteTracking(const VrTrackingState &tracking);
    bool ReadTracking(VrTrackingState *tracking) const;
    // Host: no tracking any more (session ended); the game stops using it.
    void ClearTracking();

    // Set by the host while its OpenXR session shows frames; the game only
    // captures while it is set.
    void SetHostWantsFrames(bool wants);
    bool HostWantsFrames() const;

private:
    struct Header;
    bool Map(uint32_t game_pid, bool create);
    uint8_t *Slot(int index) const;

    void *mapping_ = nullptr;
    void *mutex_ = nullptr;
    Header *header_ = nullptr;
    int write_slot_ = -1;
};
