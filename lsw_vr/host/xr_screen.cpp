// OpenXR session in lsw_vr_host.exe (stage 1: a floating screen).
//
// The loop paces itself with xrWaitFrame at the headset's rate. Whenever the
// game has published a newer frame it is uploaded and copied into the quad
// layer's swapchain; on other headset frames the layer is resubmitted as is
// (the runtime shows the swapchain's most recently released image), so the
// game's 30/60 fps and the headset's refresh rate never have to match.
//
// Head tracking: every headset frame the head pose (VIEW in LOCAL, predicted
// for display) and the headset's field of view go to the game through the
// channel. A game frame rendered with a head pose comes back with that pose
// and its field of view, and is shown as a projection layer at that pose
// (same image to both eyes, i.e. mono), so the runtime reprojects it to the
// current head pose and the world stays put even at the game's frame rate.
// Frames without a pose (menus, head tracking off) go on the floating screen.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "../src/log.h"
#include "../src/shared/frame_channel.h"
#include "../src/shared/png_write.h"
#include "xr_input.h"
#include "xr_screen.h"

namespace {
    // Screen placement in the LOCAL space (origin = head position at session start).
    constexpr float kScreenDistance = 2.2f;  // meters in front
    constexpr float kScreenWidth = 2.6f;     // meters

    const char *ResultName(XrInstance instance, XrResult result) {
        static thread_local char name[XR_MAX_RESULT_STRING_SIZE];
        name[0] = '\0';
        if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, name))) {
            sprintf_s(name, "%d", static_cast<int>(result));
        }
        return name;
    }

    IDXGIAdapter1 *FindAdapter(LUID luid) {
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
            return nullptr;
        }
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
                factory->Release();
                return adapter;
            }
            adapter->Release();
        }
        factory->Release();
        return nullptr;
    }

    template <typename T> void SafeRelease(T *&p) {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }

    // One session's worth of state; Run() returns when the session ends or fails
    // and the destructor tears everything down.
    class XrScreen {
    public:
        explicit XrScreen(FrameChannel &channel) : channel_(channel) {}

        ~XrScreen() {
            channel_.SetHostWantsFrames(false);
            channel_.ClearTracking();
            input_.Shutdown();
            DestroySwapchain();
            if (overlay_.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(overlay_.swapchain);
            SafeRelease(overlay_upload_);
            if (view_space_ != XR_NULL_HANDLE) xrDestroySpace(view_space_);
            if (space_ != XR_NULL_HANDLE) xrDestroySpace(space_);
            if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
            if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
            SafeRelease(context_);
            SafeRelease(device_);
        }

        void Run(const volatile bool &quit) {
            if (!Init()) {
                return;
            }
            bool running = false;
            bool exit = false;
            DWORD stats_tick = GetTickCount();
            while (!exit && !quit) {
                XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
                    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                        const auto state = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
                        Log("vr: session state %d", static_cast<int>(state));
                        focused_ = state == XR_SESSION_STATE_FOCUSED;
                        if (state == XR_SESSION_STATE_READY) {
                            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            running = Check(xrBeginSession(session_, &begin), "xrBeginSession");
                        } else if (state == XR_SESSION_STATE_STOPPING) {
                            xrEndSession(session_);
                            running = false;
                            channel_.ClearTracking();
                        } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                            exit = true;
                        }
                    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                        exit = true;
                    }
                    event = {XR_TYPE_EVENT_DATA_BUFFER};
                }
                if (!running) {
                    channel_.SetHostWantsFrames(false);
                    Sleep(50);
                    continue;
                }
                if (!Frame()) {
                    exit = true;
                }
                if (GetTickCount() - stats_tick >= 10000) {
                    Log("vr: %u headset frames, %u game frames shown in the last 10 s", headset_frames_, game_frames_);
                    LogEyePoses();
                    headset_frames_ = game_frames_ = 0;
                    stats_tick = GetTickCount();
                }
            }
            Log("vr: session ended");
        }

    private:
        bool Check(XrResult result, const char *what) {
            if (XR_FAILED(result)) {
                Log("vr: %s failed: %s", what, ResultName(instance_, result));
                return false;
            }
            return true;
        }

        bool Init() {
            const char *extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
            XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
            strcpy_s(instance_info.applicationInfo.applicationName, "LEGO Star Wars VR");
            instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
            instance_info.enabledExtensionCount = 1;
            instance_info.enabledExtensionNames = extensions;
            if (!Check(xrCreateInstance(&instance_info, &instance_), "xrCreateInstance")) return false;

            XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
            system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            if (!Check(xrGetSystem(instance_, &system_info, &system_), "xrGetSystem (headset connected?)")) return false;

            PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
            if (!Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                                             reinterpret_cast<PFN_xrVoidFunction *>(&get_requirements)),
                       "xrGetInstanceProcAddr")) {
                return false;
            }
            XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            if (!Check(get_requirements(instance_, system_, &requirements), "xrGetD3D11GraphicsRequirementsKHR")) {
                return false;
            }
            IDXGIAdapter1 *adapter = FindAdapter(requirements.adapterLuid);
            if (adapter == nullptr) {
                Log("vr: no DXGI adapter matches the runtime's LUID");
                return false;
            }
            const D3D_FEATURE_LEVEL level = requirements.minFeatureLevel;
            const HRESULT created = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1,
                                                      D3D11_SDK_VERSION, &device_, nullptr, &context_);
            adapter->Release();
            if (FAILED(created)) {
                Log("vr: D3D11CreateDevice failed 0x%08lx", created);
                return false;
            }

            XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            binding.device = device_;
            XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
            session_info.next = &binding;
            session_info.systemId = system_;
            if (!Check(xrCreateSession(instance_, &session_info, &session_), "xrCreateSession")) return false;

            XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            space_info.poseInReferenceSpace.orientation.w = 1.0f;
            if (!Check(xrCreateReferenceSpace(session_, &space_info, &space_), "xrCreateReferenceSpace")) return false;
            space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            if (!Check(xrCreateReferenceSpace(session_, &space_info, &view_space_), "xrCreateReferenceSpace(VIEW)")) {
                return false;
            }

            if (!ChooseFormat()) return false;
            if (!input_.Init(instance_, session_)) {
                Log("vr: controller input unavailable; continuing without it");
            }

            XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
            xrGetInstanceProperties(instance_, &properties);
            XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
            xrGetSystemProperties(instance_, system_, &system_properties);
            Log("vr: runtime %s, headset %s, swapchain format %d", properties.runtimeName,
                system_properties.systemName, static_cast<int>(swapchain_format_));
            return true;
        }

        // The game's pixels are gamma-encoded BGRA; an sRGB swapchain makes the
        // runtime read them correctly. Prefer BGRA so no swizzle is needed.
        bool ChooseFormat() {
            uint32_t count = 0;
            if (!Check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) {
                return false;
            }
            std::vector<int64_t> formats(count);
            xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
            for (const DXGI_FORMAT wanted : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
                for (int64_t format : formats) {
                    if (format == wanted) {
                        swapchain_format_ = wanted;
                        upload_format_ = wanted == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                                   : DXGI_FORMAT_R8G8B8A8_UNORM;
                        return true;
                    }
                }
            }
            Log("vr: runtime offers no 8-bit sRGB swapchain format");
            return false;
        }

        void DestroySwapchain() {
            for (EyeImage &eye : eyes_) {
                if (eye.swapchain != XR_NULL_HANDLE) {
                    xrDestroySwapchain(eye.swapchain);
                }
                eye = EyeImage();
            }
            SafeRelease(upload_);
        }

        bool EnsureSwapchain(uint32_t width, uint32_t height) {
            if (eyes_[0].swapchain != XR_NULL_HANDLE && width == width_ && height == height_) {
                return true;
            }
            DestroySwapchain();
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                              XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            info.format = swapchain_format_;
            info.sampleCount = 1;
            info.width = width;
            info.height = height;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            uint32_t count = 0;
            for (EyeImage &eye : eyes_) {
                if (!Check(xrCreateSwapchain(session_, &info, &eye.swapchain), "xrCreateSwapchain")) return false;
                xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
                eye.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                           reinterpret_cast<XrSwapchainImageBaseHeader *>(eye.images.data()));
            }

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = upload_format_;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &upload_))) {
                Log("vr: upload texture creation failed");
                return false;
            }
            width_ = width;
            height_ = height;
            Log("vr: swapchains %ux%u (one per eye), %u images each", width, height, count);
            return true;
        }

        // Copies the newest game frame (if any) into the swapchain.
        bool UploadLatest() {
            uint32_t width = 0;
            uint32_t height = 0;
            VrFrameMeta meta;
            const uint8_t *pixels = channel_.BeginRead(last_sequence_, &width, &height, &last_sequence_, &meta);
            if (pixels == nullptr) {
                return true;
            }
            bool ok = EnsureSwapchain(width, height);
            if (ok) {
                if (upload_format_ == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    swizzled_.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
                    for (size_t i = 0; i < swizzled_.size(); i += 4) {
                        std::swap(swizzled_[i], swizzled_[i + 2]);
                    }
                    pixels = swizzled_.data();
                }
                context_->UpdateSubresource(upload_, 0, nullptr, pixels, width * 4, 0);
            }
            CaptureForDump(pixels, width, height, meta);
            channel_.EndRead();
            if (!ok) {
                return false;
            }

            // Which eye(s) this frame is for. A floating-screen frame uses the
            // left slot and makes the right one stale, so stereo restarts
            // cleanly afterwards.
            meta = ValidMeta(meta);
            const bool to_left = meta.has_pose != kVrFrameRightEye;
            const bool to_right = meta.has_pose == kVrFrameMono || meta.has_pose == kVrFrameRightEye;
            for (int e = 0; e < 2; ++e) {
                if (!(e == 0 ? to_left : to_right)) {
                    continue;
                }
                EyeImage &eye = eyes_[e];
                uint32_t index = 0;
                if (!Check(xrAcquireSwapchainImage(eye.swapchain, nullptr, &index), "xrAcquireSwapchainImage")) {
                    return false;
                }
                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = XR_INFINITE_DURATION;
                if (!Check(xrWaitSwapchainImage(eye.swapchain, &wait), "xrWaitSwapchainImage")) return false;
                context_->CopyResource(eye.images[index].texture, upload_);
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                if (!Check(xrReleaseSwapchainImage(eye.swapchain, &release), "xrReleaseSwapchainImage")) return false;
                eye.has_image = true;
                eye.meta = meta;
            }
            if (meta.has_pose == kVrFrameScreen) {
                eyes_[1].has_image = false;
            }
            screen_mode_ = meta.has_pose == kVrFrameScreen;
            ++game_frames_;
            return true;
        }

        // Puts the floating screen kScreenDistance in front of the head's
        // current yaw, at eye height, facing the user.
        void PlaceQuad() {
            const XrQuaternionf &q = last_head_.orientation;
            // Head forward = q * (0, 0, -1), flattened to the horizontal plane.
            const float fx = -2.0f * (q.x * q.z + q.w * q.y);
            const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const float length = std::sqrt(fx * fx + fz * fz);
            const float dx = length > 1e-4f ? fx / length : 0.0f;
            const float dz = length > 1e-4f ? fz / length : -1.0f;
            // Yaw that turns -Z onto (dx, 0, dz): R_y(a) * (0,0,-1) = (-sin a, 0, -cos a).
            const float yaw = std::atan2(-dx, -dz);
            quad_pose_.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            quad_pose_.position = {last_head_.position.x + dx * kScreenDistance, last_head_.position.y,
                                   last_head_.position.z + dz * kScreenDistance};
            quad_placed_ = true;
        }

        // A frame's pose and field of view reach xrEndFrame, where invalid
        // values would end the session: normalise the orientation and fall
        // back to the floating screen if anything is off.
        static VrFrameMeta ValidMeta(VrFrameMeta meta) {
            if (meta.has_pose < kVrFrameMono || meta.has_pose > kVrFrameRightEye) {
                meta.has_pose = kVrFrameScreen;
                return meta;
            }
            float *q = meta.pose.orientation;
            const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            const VrFovTangents &f = meta.fov;
            bool ok = std::isfinite(length) && length > 0.5f && f.left < f.right && f.down < f.up;
            for (float v : {meta.pose.position[0], meta.pose.position[1], meta.pose.position[2], f.left, f.right, f.up,
                            f.down}) {
                ok = ok && std::isfinite(v);
            }
            if (!ok) {
                meta.has_pose = kVrFrameScreen;
                return meta;
            }
            for (int i = 0; i < 4; ++i) {
                q[i] /= length;
            }
            return meta;
        }

        // Head pose, headset field of view and controllers for the game,
        // predicted for when this headset frame is displayed.
        void PublishHead(XrTime display_time) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = display_time;
            locate.space = space_;
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t view_count = 0;
            if (XR_FAILED(xrLocateViews(session_, &locate, &view_state, 2, &view_count, views)) || view_count != 2 ||
                (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
                return;
            }
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xrLocateSpace(view_space_, space_, display_time, &head)) ||
                (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
                return;
            }
            if (!logged_views_) {
                const XrVector3f &l = views[0].pose.position, &r = views[1].pose.position;
                const float ipd = std::sqrt((l.x - r.x) * (l.x - r.x) + (l.y - r.y) * (l.y - r.y) + (l.z - r.z) * (l.z - r.z));
                for (int e = 0; e < 2; ++e) {
                    Log("vr: eye %d fov deg left %.1f right %.1f up %.1f down %.1f", e,
                        views[e].fov.angleLeft * 57.2958f, views[e].fov.angleRight * 57.2958f,
                        views[e].fov.angleUp * 57.2958f, views[e].fov.angleDown * 57.2958f);
                }
                Log("vr: eye separation %.1f mm", ipd * 1000.0f);
                logged_views_ = true;
            }
            VrTrackingState tracking = {};
            VrHeadState &state = tracking.head;
            last_head_ = head.pose;
            last_views_[0] = views[0].pose;
            last_views_[1] = views[1].pose;
            native_fov_[0] = views[0].fov;
            native_fov_[1] = views[1].fov;
            has_native_fov_ = true;
            has_head_ = true;
            state.pose.orientation[0] = head.pose.orientation.x;
            state.pose.orientation[1] = head.pose.orientation.y;
            state.pose.orientation[2] = head.pose.orientation.z;
            state.pose.orientation[3] = head.pose.orientation.w;
            const bool has_position = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            state.pose.position[0] = has_position ? head.pose.position.x : 0.0f;
            state.pose.position[1] = has_position ? head.pose.position.y : 0.0f;
            state.pose.position[2] = has_position ? head.pose.position.z : 0.0f;
            // Union of both eyes' fields of view.
            state.fov.left = std::tan(std::min(views[0].fov.angleLeft, views[1].fov.angleLeft));
            state.fov.right = std::tan(std::max(views[0].fov.angleRight, views[1].fov.angleRight));
            state.fov.up = std::tan(std::max(views[0].fov.angleUp, views[1].fov.angleUp));
            state.fov.down = std::tan(std::min(views[0].fov.angleDown, views[1].fov.angleDown));
            for (int e = 0; e < 2; ++e) {
                const XrPosef &eye = views[e].pose;
                tracking.eyes[e] = {{eye.orientation.x, eye.orientation.y, eye.orientation.z, eye.orientation.w},
                                    {eye.position.x, eye.position.y, eye.position.z}};
            }
            input_.Read(session_, space_, display_time, focused_, tracking.hands);
            tracking.host_tick = GetTickCount();
            channel_.WriteTracking(tracking);
        }

        // Live tuning from lsw_vr_tuner.exe (lsw_vr_tune.txt next to this exe):
        // separation turns each eye's image outward (+) or inward (-) by that
        // many degrees.
        void PollTuneFile() {
            char path[MAX_PATH];
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            char *slash = strrchr(path, '\\');
            if (slash == nullptr) {
                return;
            }
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "lsw_vr_tune.txt");
            WIN32_FILE_ATTRIBUTE_DATA info;
            if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info) ||
                CompareFileTime(&info.ftLastWriteTime, &tune_time_) == 0) {
                return;
            }
            tune_time_ = info.ftLastWriteTime;
            FILE *file = nullptr;
            if (fopen_s(&file, path, "r") != 0 || file == nullptr) {
                return;
            }
            char line[128];
            float separation = 0.0f, depth = 1.0f, scale = 0.3f, height = 0.03f;
            int stereo = 1;
            while (fgets(line, sizeof(line), file) != nullptr) {
                float value = 0.0f;
                int flag = 0;
                if (sscanf_s(line, "separation=%f", &value) == 1 && std::isfinite(value) && std::fabs(value) <= 10.0f) {
                    separation = value;
                } else if (sscanf_s(line, "stereo_strength=%f", &value) == 1) {
                    depth = value;
                } else if (sscanf_s(line, "world_scale=%f", &value) == 1) {
                    scale = value;
                } else if (sscanf_s(line, "eye_lift=%f", &value) == 1) {
                    height = value;
                } else if (sscanf_s(line, "stereo=%d", &flag) == 1) {
                    stereo = flag;
                } else if (sscanf_s(line, "dump=%d", &flag) == 1) {
                    if (tune_seen_ && flag != dump_request_) {
                        dump_pending_ = true;
                        dump_have_[0] = dump_have_[1] = false;
                        Log("vr: dump requested");
                    }
                    dump_request_ = flag;
                }
            }
            fclose(file);
            separation_ = separation * 3.14159265f / 180.0f;
            Log("vr: tuner separation %+.2f deg, depth %.1f, scale %.2f, %s", separation, depth, scale,
                stereo ? "stereo" : "mono");
            // Show the values in the headset for a few seconds (not for the
            // file as found at startup).
            if (tune_seen_) {
                char text[160];
                sprintf_s(text, "Separation %+.2f    Depth %.1f\nScale %.2f   Height %+.2f   %s", separation, depth,
                          scale, height, stereo ? "STEREO" : "MONO");
                DrawOverlay(text);
                overlay_until_ = GetTickCount64() + 4000;
            }
            tune_seen_ = true;
        }

        // Diagnostics: the poses the two shown eye images were rendered at,
        // compared with each other and with the headset's eyes now.
        static void Describe(char *out, size_t size, const char *name, const VrPose &p) {
            sprintf_s(out, size, "%s pos %.4f %.4f %.4f quat %.4f %.4f %.4f %.4f", name, p.position[0], p.position[1],
                      p.position[2], p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]);
        }

        static float AngleBetween(const float *a, const float *b) {
            const float dot = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
            return 2.0f * std::acos(std::min(1.0f, dot)) * 57.2958f;
        }

        void LogEyePoses() {
            if (!eyes_[0].has_image || !eyes_[1].has_image) {
                return;
            }
            const VrFrameMeta &l = eyes_[0].meta, &r = eyes_[1].meta;
            const float dx = r.pose.position[0] - l.pose.position[0], dy = r.pose.position[1] - l.pose.position[1],
                        dz = r.pose.position[2] - l.pose.position[2];
            Log("vr: shown eyes kinds %d/%d, left->right %.1f mm (x %.1f y %.1f z %.1f), rotation between %.2f deg",
                l.has_pose, r.has_pose, std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f, dx * 1000.0f, dy * 1000.0f,
                dz * 1000.0f, AngleBetween(l.pose.orientation, r.pose.orientation));
        }

        void CaptureForDump(const uint8_t *pixels, uint32_t width, uint32_t height, const VrFrameMeta &meta) {
            if (!dump_pending_ || (meta.has_pose != kVrFrameLeftEye && meta.has_pose != kVrFrameRightEye)) {
                return;
            }
            const int e = meta.has_pose == kVrFrameLeftEye ? 0 : 1;
            if (dump_have_[e]) {
                return;
            }
            dump_pixels_[e].assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
            dump_meta_[e] = meta;
            dump_width_ = width;
            dump_height_ = height;
            dump_have_[e] = true;
            if (!dump_have_[0] || !dump_have_[1]) {
                return;
            }
            dump_pending_ = false;
            char path[MAX_PATH];
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            char *slash = strrchr(path, '\\');
            if (slash == nullptr) {
                return;
            }
            const std::string folder(path, slash + 1);
            for (int i = 0; i < 2; ++i) {
                // Half size, BGRA -> RGB.
                const int w = static_cast<int>(dump_width_ / 2), h = static_cast<int>(dump_height_ / 2);
                std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const uint8_t *in = &dump_pixels_[i][(static_cast<size_t>(y) * 2 * dump_width_ + x * 2) * 4];
                        uint8_t *out = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                        out[0] = in[2];
                        out[1] = in[1];
                        out[2] = in[0];
                    }
                }
                png_write::WritePng(folder + (i == 0 ? "vr_dump_left.png" : "vr_dump_right.png"), rgb, w, h);
            }
            FILE *info = nullptr;
            if (fopen_s(&info, (folder + "vr_dump.txt").c_str(), "w") == 0 && info != nullptr) {
                char line[256];
                for (int i = 0; i < 2; ++i) {
                    const VrFrameMeta &m = dump_meta_[i];
                    Describe(line, sizeof(line), i == 0 ? "image left " : "image right", m.pose);
                    fprintf(info, "%s kind %d fov %.3f %.3f %.3f %.3f\n", line, m.has_pose, m.fov.left, m.fov.right,
                            m.fov.up, m.fov.down);
                }
                for (int i = 0; i < 2; ++i) {
                    const VrPose now = {{last_views_[i].orientation.x, last_views_[i].orientation.y,
                                         last_views_[i].orientation.z, last_views_[i].orientation.w},
                                        {last_views_[i].position.x, last_views_[i].position.y,
                                         last_views_[i].position.z}};
                    Describe(line, sizeof(line), i == 0 ? "headset left " : "headset right", now);
                    fprintf(info, "%s\n", line);
                }
                fprintf(info, "separation %.2f deg\n", separation_ * 57.2958f);
                fclose(info);
            }
            Log("vr: dump written (vr_dump_left.png, vr_dump_right.png, vr_dump.txt)");
        }

        // Head-locked text panel (tuner values): drawn with GDI into a small
        // swapchain shown as a quad below the centre of view.
        static constexpr int kOverlayWidth = 768;
        static constexpr int kOverlayHeight = 160;

        void DrawOverlay(const char *text) {
            if (session_ == XR_NULL_HANDLE || upload_format_ == DXGI_FORMAT_UNKNOWN) {
                return;
            }
            if (overlay_.swapchain == XR_NULL_HANDLE) {
                XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                  XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                info.format = swapchain_format_;
                info.sampleCount = 1;
                info.width = kOverlayWidth;
                info.height = kOverlayHeight;
                info.faceCount = 1;
                info.arraySize = 1;
                info.mipCount = 1;
                if (!Check(xrCreateSwapchain(session_, &info, &overlay_.swapchain), "xrCreateSwapchain(overlay)")) {
                    overlay_.swapchain = XR_NULL_HANDLE;
                    return;
                }
                uint32_t count = 0;
                xrEnumerateSwapchainImages(overlay_.swapchain, 0, &count, nullptr);
                overlay_.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                xrEnumerateSwapchainImages(overlay_.swapchain, count, &count,
                                           reinterpret_cast<XrSwapchainImageBaseHeader *>(overlay_.images.data()));
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = kOverlayWidth;
                desc.Height = kOverlayHeight;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = upload_format_;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                if (FAILED(device_->CreateTexture2D(&desc, nullptr, &overlay_upload_))) {
                    overlay_upload_ = nullptr;
                    return;
                }
            }
            if (overlay_upload_ == nullptr) {
                return;
            }
            // GDI text into a top-down 32-bit DIB.
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = kOverlayWidth;
            bmi.bmiHeader.biHeight = -kOverlayHeight;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            void *bits = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap == nullptr || bits == nullptr) {
                DeleteDC(dc);
                return;
            }
            HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
            RECT rect = {0, 0, kOverlayWidth, kOverlayHeight};
            HBRUSH background = CreateSolidBrush(RGB(20, 20, 30));
            FillRect(dc, &rect, background);
            DeleteObject(background);
            HFONT font = CreateFontA(-52, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                                     "Segoe UI");
            HGDIOBJ old_font = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 220, 80));
            RECT measure = rect;
            DrawTextA(dc, text, -1, &measure, DT_CENTER | DT_NOPREFIX | DT_CALCRECT);
            RECT target = rect;
            target.top = (kOverlayHeight - (measure.bottom - measure.top)) / 2;
            DrawTextA(dc, text, -1, &target, DT_CENTER | DT_NOPREFIX);
            GdiFlush();
            auto *pixels = static_cast<uint8_t *>(bits);
            const size_t total = static_cast<size_t>(kOverlayWidth) * kOverlayHeight * 4;
            for (size_t i = 0; i < total; i += 4) {
                if (upload_format_ == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    std::swap(pixels[i], pixels[i + 2]);
                }
                pixels[i + 3] = 255;
            }
            context_->UpdateSubresource(overlay_upload_, 0, nullptr, pixels, kOverlayWidth * 4, 0);
            SelectObject(dc, old_font);
            DeleteObject(font);
            SelectObject(dc, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);

            uint32_t index = 0;
            if (!Check(xrAcquireSwapchainImage(overlay_.swapchain, nullptr, &index), "xrAcquireSwapchainImage")) return;
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(overlay_.swapchain, &wait))) {
                context_->CopyResource(overlay_.images[index].texture, overlay_upload_);
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(overlay_.swapchain, &release);
            overlay_.has_image = true;
        }

        bool Frame() {
            if (headset_frames_ % 20 == 0) {
                PollTuneFile();
            }
            XrFrameState frame_state{XR_TYPE_FRAME_STATE};
            if (!Check(xrWaitFrame(session_, nullptr, &frame_state), "xrWaitFrame")) return false;
            // Frames cost the game a GPU sync, so only ask while the headset shows them.
            channel_.SetHostWantsFrames(frame_state.shouldRender == XR_TRUE);
            if (!Check(xrBeginFrame(session_, nullptr), "xrBeginFrame")) return false;
            PublishHead(frame_state.predictedDisplayTime);

            if (frame_state.shouldRender && !UploadLatest()) {
                return false;
            }

            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.space = space_;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = eyes_[0].swapchain;
            quad.subImage.imageRect.extent = {static_cast<int32_t>(width_), static_cast<int32_t>(height_)};

            quad.size.width = kScreenWidth;
            quad.size.height = width_ > 0 ? kScreenWidth * height_ / width_ : 0.0f;
            // World-locked view: each eye's latest image at the pose and field
            // of view it was rendered with (an eye without its own image yet
            // borrows the other's).
            XrCompositionLayerProjectionView views[2];
            for (int e = 0; e < 2; ++e) {
                XrCompositionLayerProjectionView &view = views[e];
                const EyeImage &eye = eyes_[e].has_image ? eyes_[e] : eyes_[1 - e];
                const VrFrameMeta &shown = eye.meta;
                view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                const VrPose &pose = shown.pose;
                view.pose.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2],
                                         pose.orientation[3]};
                view.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
                // The game renders a symmetric view covering both eyes' fields
                // of view. Each eye is given the part of it that matches the
                // headset's own (asymmetric) field of view for that eye, so the
                // runtime never has to honour a non-native one. Separation
                // moves the crop window, which moves the picture by that angle.
                const float shift = e == 0 ? -separation_ : separation_;
                const XrFovf want = has_native_fov_ ? native_fov_[e]
                                                    : XrFovf{std::atan(shown.fov.left), std::atan(shown.fov.right),
                                                             std::atan(shown.fov.up), std::atan(shown.fov.down)};
                const float span_x = shown.fov.right - shown.fov.left;
                const float span_y = shown.fov.up - shown.fov.down;
                // Image tangent -> pixel (x from the left, y from the top).
                auto px = [&](float angle) {
                    const float t = std::clamp(std::tan(angle), shown.fov.left, shown.fov.right);
                    return static_cast<int32_t>(std::lround((t - shown.fov.left) / span_x * width_));
                };
                auto py = [&](float angle) {
                    const float t = std::clamp(std::tan(angle), shown.fov.down, shown.fov.up);
                    return static_cast<int32_t>(std::lround((shown.fov.up - t) / span_y * height_));
                };
                int32_t x0 = px(want.angleLeft - shift), x1 = px(want.angleRight - shift);
                const int32_t y0 = py(want.angleUp), y1 = py(want.angleDown);
                if (x1 - x0 < 16 || y1 - y0 < 16) {
                    x0 = 0;
                    x1 = static_cast<int32_t>(width_);
                }
                // Declare exactly the angles the (rounded) pixel edges cover.
                auto ax = [&](int32_t x) { return std::atan(shown.fov.left + span_x * x / width_) + shift; };
                auto ay = [&](int32_t y) { return std::atan(shown.fov.up - span_y * y / height_); };
                view.fov.angleLeft = ax(x0);
                view.fov.angleRight = ax(x1);
                view.fov.angleUp = ay(y0);
                view.fov.angleDown = ay(y1);
                view.subImage.swapchain = eye.swapchain;
                view.subImage.imageRect.offset = {x0, y0};
                view.subImage.imageRect.extent = {x1 - x0, y1 - y0};
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = space_;
            projection.viewCount = 2;
            projection.views = views;

            // The floating screen appears in front of wherever the user looks
            // when it comes up (menus), at eye level, facing them; it then
            // stays put while it is shown.
            if (screen_mode_ && (!quad_placed_ || last_shown_pose_) && has_head_) {
                PlaceQuad();
            }
            last_shown_pose_ = !screen_mode_;
            quad.pose = quad_pose_;

            // Tuner values, head-locked below the centre of view.
            XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
            overlay.space = view_space_;
            overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            overlay.subImage.swapchain = overlay_.swapchain;
            overlay.subImage.imageRect.extent = {kOverlayWidth, kOverlayHeight};
            overlay.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            overlay.pose.position = {0.0f, -0.25f, -1.2f};
            overlay.size.width = 0.8f;
            overlay.size.height = 0.8f * kOverlayHeight / kOverlayWidth;
            const bool show_overlay = overlay_.has_image && GetTickCount64() < overlay_until_;

            const XrCompositionLayerBaseHeader *layers[] = {
                screen_mode_ ? reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad)
                             : reinterpret_cast<XrCompositionLayerBaseHeader *>(&projection),
                reinterpret_cast<XrCompositionLayerBaseHeader *>(&overlay)};

            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frame_state.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount = (frame_state.shouldRender && eyes_[0].has_image) ? (show_overlay ? 2 : 1) : 0;
            end.layers = layers;
            if (!Check(xrEndFrame(session_, &end), "xrEndFrame")) return false;
            ++headset_frames_;
            return true;
        }

        FrameChannel &channel_;
        XrInstance instance_ = XR_NULL_HANDLE;
        XrSystemId system_ = XR_NULL_SYSTEM_ID;
        XrSession session_ = XR_NULL_HANDLE;
        XrSpace space_ = XR_NULL_HANDLE;
        XrSpace view_space_ = XR_NULL_HANDLE;
        XrInput input_;
        bool focused_ = false;
        // One swapchain per eye, each with the metadata of the image it holds.
        struct EyeImage {
            XrSwapchain swapchain = XR_NULL_HANDLE;
            std::vector<XrSwapchainImageD3D11KHR> images;
            bool has_image = false;
            VrFrameMeta meta = {};
        };
        EyeImage eyes_[2];
        bool screen_mode_ = true;  // last frame was for the floating screen
        bool logged_views_ = false;
        XrPosef quad_pose_ = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -kScreenDistance}};
        bool quad_placed_ = false;
        bool last_shown_pose_ = false;
        XrPosef last_head_ = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        bool has_head_ = false;
        DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT upload_format_ = DXGI_FORMAT_UNKNOWN;
        ID3D11Device *device_ = nullptr;
        ID3D11DeviceContext *context_ = nullptr;
        ID3D11Texture2D *upload_ = nullptr;
        std::vector<uint8_t> swizzled_;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint64_t last_sequence_ = 0;
        unsigned headset_frames_ = 0;
        FILETIME tune_time_ = {};
        float separation_ = 0.0f;  // radians per eye, + = apart
        bool tune_seen_ = false;
        int dump_request_ = 0;
        bool dump_pending_ = false;
        bool dump_have_[2] = {};
        std::vector<uint8_t> dump_pixels_[2];
        VrFrameMeta dump_meta_[2] = {};
        uint32_t dump_width_ = 0, dump_height_ = 0;
        XrPosef last_views_[2] = {};
        XrFovf native_fov_[2] = {};
        bool has_native_fov_ = false;
        EyeImage overlay_;
        ID3D11Texture2D *overlay_upload_ = nullptr;
        ULONGLONG overlay_until_ = 0;
        unsigned game_frames_ = 0;
    };
} // namespace

void RunXrScreen(FrameChannel &channel, const volatile bool &quit) {
    XrScreen screen(channel);
    screen.Run(quit);
}
