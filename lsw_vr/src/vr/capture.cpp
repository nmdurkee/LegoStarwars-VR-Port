// Back buffer capture on the game's render thread.
//
// Transport, stage 1: GPU -> system memory -> shared memory -> host's D3D11
// upload. Plain
// D3D9 (non-Ex) devices can't share textures with D3D11, and switching the
// game to D3D9Ex would break its managed-pool resources, so for the mono
// screen the frame takes the CPU route. GetRenderTargetData waits for the GPU
// to finish the frame, which costs a few ms at 1280x800; stereo will need a
// GPU-side transport instead.

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <vector>

#include "../log.h"
#include "../shared/frame_channel.h"
#include "vr.h"
#include "sim.h"
#include "vr_channel.h"

namespace {
    IDirect3DSurface9 *g_resolve = nullptr;  // non-MSAA copy, only when the back buffer is multisampled
    IDirect3DSurface9 *g_readback = nullptr; // system-memory copy we can lock
    // Desktop mirror: with stereo by alternating eyes the window would show
    // left and right eye frames alternately (a doubled, blurry picture), so the
    // left eye's frame is kept and shown again over each right eye frame.
    IDirect3DSurface9 *g_mirror = nullptr;
    bool g_mirror_valid = false;
    D3DSURFACE_DESC g_desc = {};
    IDirect3DDevice9 *g_device = nullptr;  // surfaces belong to this device
    bool g_warned_format = false;
    bool g_warned_size = false;

    void ReleaseSurfaces() {
        if (g_resolve != nullptr) {
            g_resolve->Release();
            g_resolve = nullptr;
        }
        if (g_readback != nullptr) {
            g_readback->Release();
            g_readback = nullptr;
        }
        if (g_mirror != nullptr) {
            g_mirror->Release();
            g_mirror = nullptr;
        }
        g_mirror_valid = false;
    }

    bool EnsureSurfaces(IDirect3DDevice9 *device, const D3DSURFACE_DESC &desc) {
        if (g_readback != nullptr && device == g_device && desc.Width == g_desc.Width && desc.Height == g_desc.Height &&
            desc.Format == g_desc.Format && desc.MultiSampleType == g_desc.MultiSampleType) {
            return true;
        }
        ReleaseSurfaces();
        g_desc = desc;
        g_device = device;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE &&
            FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &g_resolve, nullptr))) {
            Log("vr: capture resolve surface failed");
            return false;
        }
        if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                       &g_readback, nullptr))) {
            Log("vr: capture readback surface failed");
            ReleaseSurfaces();
            return false;
        }
        if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                              &g_mirror, nullptr))) {
            g_mirror = nullptr;  // optional: the window then shows both eyes
        }
        Log("vr: capturing %ux%u format %d msaa %d", desc.Width, desc.Height, desc.Format, desc.MultiSampleType);
        return true;
    }
} // namespace

void VrCaptureFrame(IDirect3DDevice9 *device, const VrFrameMeta &meta) {
    FrameChannel *channel = VrChannel();
    const bool sim = SimActive();
    if (!sim && (channel == nullptr || !channel->HostWantsFrames())) {
        return;
    }
    IDirect3DSurface9 *back_buffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer))) {
        return;
    }
    D3DSURFACE_DESC desc;
    back_buffer->GetDesc(&desc);
    // D3DFMT_A8R8G8B8 / X8R8G8B8 are BGRA in memory, which is what the VR side takes.
    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
        if (!g_warned_format) {
            Log("vr: unsupported back buffer format %d, not capturing", desc.Format);
            g_warned_format = true;
        }
        back_buffer->Release();
        return;
    }
    if (!EnsureSurfaces(device, desc)) {
        back_buffer->Release();
        return;
    }
    IDirect3DSurface9 *source = back_buffer;
    if (g_resolve != nullptr && SUCCEEDED(device->StretchRect(back_buffer, nullptr, g_resolve, nullptr, D3DTEXF_NONE))) {
        source = g_resolve;
    }
    const HRESULT copied = device->GetRenderTargetData(source, g_readback);
    if (g_mirror != nullptr && meta.has_pose == kVrFrameLeftEye) {
        g_mirror_valid = SUCCEEDED(device->StretchRect(source, nullptr, g_mirror, nullptr, D3DTEXF_NONE));
    } else if (g_mirror != nullptr && meta.has_pose == kVrFrameRightEye && g_mirror_valid) {
        device->StretchRect(g_mirror, nullptr, back_buffer, nullptr, D3DTEXF_NONE);  // the window keeps the left eye
    } else if (meta.has_pose != kVrFrameRightEye) {
        g_mirror_valid = false;
    }
    back_buffer->Release();
    if (FAILED(copied)) {
        return;
    }

    D3DLOCKED_RECT locked;
    if (FAILED(g_readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        return;
    }
    if ((desc.Width > FrameChannel::kMaxWidth || desc.Height > FrameChannel::kMaxHeight) && !g_warned_size) {
        Log("vr: %ux%u is larger than the %ux%u the headset link carries; not capturing", desc.Width, desc.Height,
            FrameChannel::kMaxWidth, FrameChannel::kMaxHeight);
        g_warned_size = true;
    }
    if (sim) {
        // Simulation: the frame goes to the dump buffers instead of the host.
        static std::vector<uint8_t> frame;
        frame.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
        const auto *src = static_cast<const uint8_t *>(locked.pBits);
        for (UINT y = 0; y < desc.Height; ++y) {
            memcpy(frame.data() + static_cast<size_t>(y) * desc.Width * 4, src + static_cast<size_t>(y) * locked.Pitch,
                   static_cast<size_t>(desc.Width) * 4);
        }
        g_readback->UnlockRect();
        SimOnFrame(frame.data(), desc.Width, desc.Height, meta);
        return;
    }
    uint8_t *pixels = channel->BeginWrite(desc.Width, desc.Height);
    if (pixels != nullptr) {
        const auto *src = static_cast<const uint8_t *>(locked.pBits);
        for (UINT y = 0; y < desc.Height; ++y) {
            // Copy with alpha forced opaque: the game's destination alpha is
            // undefined (X8R8G8B8 or leftover values).
            const auto *in = reinterpret_cast<const uint32_t *>(src + static_cast<size_t>(y) * locked.Pitch);
            auto *out = reinterpret_cast<uint32_t *>(pixels) + static_cast<size_t>(y) * desc.Width;
            for (UINT x = 0; x < desc.Width; ++x) {
                out[x] = in[x] | 0xff000000u;
            }
        }
        channel->EndWrite(meta);
    }
    g_readback->UnlockRect();
}

void VrOnDeviceReset() {
    ReleaseSurfaces();
}
