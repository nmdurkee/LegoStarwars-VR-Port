// 32-bit OpenXR smoke test: opens a D3D11 session on the headset and submits
// a few seconds of frames that alternate green/blue, proving that a 32-bit
// process can drive the Rift through Meta's OpenXR runtime.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK_XR(call)                                                                  \
    do {                                                                                \
        XrResult res_ = (call);                                                         \
        if (XR_FAILED(res_)) {                                                          \
            char name_[XR_MAX_RESULT_STRING_SIZE] = "?";                                \
            if (g_instance != XR_NULL_HANDLE) xrResultToString(g_instance, res_, name_); \
            std::printf("FAIL: %s -> %s (%d)\n", #call, name_, (int)res_);              \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static XrInstance g_instance = XR_NULL_HANDLE;

static IDXGIAdapter1 *FindAdapter(LUID luid) {
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) return nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (memcmp(&desc.AdapterLuid, &luid, sizeof(luid)) == 0) {
            std::printf("GPU: %ls\n", desc.Description);
            factory->Release();
            return adapter;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Process pointer size: %u bytes (%s)\n", (unsigned)sizeof(void *),
                sizeof(void *) == 4 ? "32-bit" : "64-bit");

    const char *extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(instance_info.applicationInfo.applicationName, "ToVR 32-bit smoke test");
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = extensions;
    CHECK_XR(xrCreateInstance(&instance_info, &g_instance));

    XrInstanceProperties instance_props{XR_TYPE_INSTANCE_PROPERTIES};
    CHECK_XR(xrGetInstanceProperties(g_instance, &instance_props));
    std::printf("Runtime: %s\n", instance_props.runtimeName);

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system_id;
    CHECK_XR(xrGetSystem(g_instance, &system_info, &system_id));

    XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
    CHECK_XR(xrGetSystemProperties(g_instance, system_id, &system_props));
    std::printf("Headset: %s (position tracking: %s)\n", system_props.systemName,
                system_props.trackingProperties.positionTracking ? "yes" : "no");

    PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
    CHECK_XR(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction *)&get_reqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    CHECK_XR(get_reqs(g_instance, system_id, &reqs));

    IDXGIAdapter1 *adapter = FindAdapter(reqs.adapterLuid);
    if (!adapter) {
        std::printf("FAIL: no DXGI adapter matches the runtime's LUID\n");
        return 1;
    }
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL level = reqs.minFeatureLevel;
    std::printf("Creating D3D11 device (min feature level 0x%x)...\n", (unsigned)level);
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
        std::printf("FAIL: D3D11CreateDevice\n");
        return 1;
    }
    std::printf("D3D11 device created; creating XR session...\n");

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = system_id;
    XrSession session;
    CHECK_XR(xrCreateSession(g_instance, &session_info, &session));
    std::printf("Session created\n");

    uint32_t view_count = 0;
    CHECK_XR(xrEnumerateViewConfigurationViews(g_instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                               &view_count, nullptr));
    std::vector<XrViewConfigurationView> config_views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    CHECK_XR(xrEnumerateViewConfigurationViews(g_instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               view_count, &view_count, config_views.data()));
    const uint32_t width = config_views[0].recommendedImageRectWidth;
    const uint32_t height = config_views[0].recommendedImageRectHeight;
    std::printf("Eyes: %u, recommended per-eye resolution %ux%u\n", view_count, width, height);

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space;
    CHECK_XR(xrCreateReferenceSpace(session, &space_info, &space));

    // One double-wide swapchain; each eye renders into its half.
    XrSwapchainCreateInfo swap_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swap_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swap_info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    swap_info.sampleCount = 1;
    swap_info.width = width * 2;
    swap_info.height = height;
    swap_info.faceCount = 1;
    swap_info.arraySize = 1;
    swap_info.mipCount = 1;
    XrSwapchain swapchain;
    CHECK_XR(xrCreateSwapchain(session, &swap_info, &swapchain));

    uint32_t image_count = 0;
    CHECK_XR(xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    CHECK_XR(xrEnumerateSwapchainImages(swapchain, image_count, &image_count,
                                        (XrSwapchainImageBaseHeader *)images.data()));
    std::vector<ID3D11RenderTargetView *> rtvs(image_count, nullptr);
    for (uint32_t i = 0; i < image_count; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(images[i].texture, &rtv_desc, &rtvs[i]);
    }

    bool running = false;
    bool done = false;
    int frames_submitted = 0;
    ULONGLONG start = GetTickCount64();
    ULONGLONG first_frame_at = 0;

    while (!done) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto *changed = (XrEventDataSessionStateChanged *)&event;
                std::printf("Session state -> %d\n", (int)changed->state);
                if (changed->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CHECK_XR(xrBeginSession(session, &begin));
                    running = true;
                } else if (changed->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                    done = true;
                } else if (changed->state == XR_SESSION_STATE_EXITING ||
                           changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                    done = true;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!running) {
            if (GetTickCount64() - start > 15000) {
                std::printf("FAIL: session never became READY (is the headset connected and awake?)\n");
                return 1;
            }
            Sleep(10);
            continue;
        }

        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        CHECK_XR(xrWaitFrame(session, nullptr, &frame_state));
        CHECK_XR(xrBeginFrame(session, nullptr));

        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frame_state.predictedDisplayTime;
        locate.space = space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        std::vector<XrView> views(view_count, {XR_TYPE_VIEW});
        CHECK_XR(xrLocateViews(session, &locate, &view_state, view_count, &view_count, views.data()));

        uint32_t image_index;
        CHECK_XR(xrAcquireSwapchainImage(swapchain, nullptr, &image_index));
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        CHECK_XR(xrWaitSwapchainImage(swapchain, &wait));

        // Alternate green and blue once per second so it is obvious in the headset.
        const bool green = ((GetTickCount64() / 1000) % 2) == 0;
        const float color[4] = {0.0f, green ? 0.6f : 0.0f, green ? 0.0f : 0.6f, 1.0f};
        context->ClearRenderTargetView(rtvs[image_index], color);

        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CHECK_XR(xrReleaseSwapchainImage(swapchain, &release));

        std::vector<XrCompositionLayerProjectionView> proj_views(view_count,
                                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        for (uint32_t i = 0; i < view_count; ++i) {
            proj_views[i].pose = views[i].pose;
            proj_views[i].fov = views[i].fov;
            proj_views[i].subImage.swapchain = swapchain;
            proj_views[i].subImage.imageRect.offset = {(int32_t)(i * width), 0};
            proj_views[i].subImage.imageRect.extent = {(int32_t)width, (int32_t)height};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = view_count;
        layer.views = proj_views.data();
        const XrCompositionLayerBaseHeader *layers[] = {(XrCompositionLayerBaseHeader *)&layer};

        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = frame_state.shouldRender ? 1 : 0;
        end.layers = layers;
        CHECK_XR(xrEndFrame(session, &end));

        if (frame_state.shouldRender) {
            if (frames_submitted == 0) first_frame_at = GetTickCount64();
            ++frames_submitted;
        }
        if (first_frame_at != 0 && GetTickCount64() - first_frame_at > 8000) {
            CHECK_XR(xrRequestExitSession(session));
            first_frame_at = ~0ull >> 1;  // only request once
        }
        if (GetTickCount64() - start > 30000) done = true;
    }

    std::printf("Frames rendered to headset: %d\n", frames_submitted);
    std::printf(frames_submitted > 0 ? "PASS\n" : "FAIL: no frames were rendered\n");

    for (auto *rtv : rtvs) rtv->Release();
    xrDestroySwapchain(swapchain);
    xrDestroySpace(space);
    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    context->Release();
    device->Release();
    adapter->Release();
    return frames_submitted > 0 ? 0 : 1;
}
