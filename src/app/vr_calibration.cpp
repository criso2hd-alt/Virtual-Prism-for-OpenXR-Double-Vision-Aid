#include "vr_calibration.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "scenes.h"

using Microsoft::WRL::ComPtr;

namespace dfx {
namespace {

void Check(XrResult r, const char* what) {
    if (XR_FAILED(r)) throw std::runtime_error(std::string(what) + " failed (XrResult " + std::to_string((int)r) + ")");
}

// Renders the per-eye images on a worker thread so the VR frame loop never stalls on GDI+.
class AsyncRenderer {
public:
    AsyncRenderer(bool swapRB, Preview* preview) : swapRB_(swapRB), preview_(preview) {
        for (auto& b : work_) b.resize((size_t)kImageSize * kImageSize);
        for (auto& b : out_) b.resize((size_t)kImageSize * kImageSize);
        thread_ = std::thread([this] { Loop(); });
    }
    ~AsyncRenderer() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        thread_.join();
    }
    void Request(const UiState& ui) {
        { std::lock_guard<std::mutex> l(m_); req_ = ui; pending_ = true; }
        cv_.notify_all();
    }
    bool Busy() { std::lock_guard<std::mutex> l(m_); return pending_ || rendering_; }
    // Calls fn(eye, pixels) for each eye if a new frame is ready.
    template <class F> bool TakeReady(F fn) {
        std::lock_guard<std::mutex> l(m_);
        if (!ready_) return false;
        ready_ = false;
        for (int e = 0; e < 2; e++) fn(e, out_[e].data());
        return true;
    }

private:
    void Loop() {
        for (;;) {
            UiState ui;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return pending_ || stop_; });
                if (stop_) return;
                ui = req_;
                pending_ = false;
                rendering_ = true;
            }
            for (int e = 0; e < 2; e++) {
                RenderEyeImage(e, ui, work_[e].data());
                if (preview_) Downscale(e);
                if (swapRB_)
                    for (auto& p : work_[e]) p = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
            }
            std::lock_guard<std::mutex> l(m_);
            for (int e = 0; e < 2; e++) work_[e].swap(out_[e]);
            ready_ = true;
            rendering_ = false;
        }
    }
    // 4x4 box filter of the freshly rendered image into the spectator preview.
    void Downscale(int e) {
        constexpr int f = kImageSize / kPreviewSize;
        std::vector<uint32_t> shrunk((size_t)kPreviewSize * kPreviewSize);
        const uint32_t* src = work_[e].data();
        for (int y = 0; y < kPreviewSize; y++)
            for (int x = 0; x < kPreviewSize; x++) {
                uint32_t b = 0, g = 0, r = 0;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) {
                        uint32_t p = src[(size_t)(y * f + j) * kImageSize + x * f + i];
                        b += p & 0xFF; g += (p >> 8) & 0xFF; r += (p >> 16) & 0xFF;
                    }
                shrunk[(size_t)y * kPreviewSize + x] =
                    0xFF000000u | ((r / (f * f)) << 16) | ((g / (f * f)) << 8) | (b / (f * f));
            }
        std::lock_guard<std::mutex> l(preview_->m);
        preview_->img[e].swap(shrunk);
        if (e == 1) preview_->hasImage = true;
    }
    bool swapRB_;
    Preview* preview_;
    std::vector<uint32_t> work_[2], out_[2];
    std::mutex m_;
    std::condition_variable cv_;
    UiState req_;
    bool pending_ = false, rendering_ = false, ready_ = false, stop_ = false;
    std::thread thread_;
};

std::vector<int> Signature(const UiState& u) {
    std::vector<int> s = {u.scene, u.stepMode, u.eyeMode, u.help ? 1 : 0, (int)(u.saveProgress * 10)};
    for (int i = 0; i < 2; i++) {
        s.push_back((int)std::lround(u.values.eye[i].hPrism * 100));
        s.push_back((int)std::lround(u.values.eye[i].vPrism * 100));
        s.push_back((int)std::lround(u.values.eye[i].rollDeg * 100));
    }
    return s;
}

float Dead(float v) {
    const float dz = 0.15f;
    float a = std::fabs(v);
    if (a < dz) return 0;
    return (v < 0 ? -1.0f : 1.0f) * (a - dz) / (1 - dz);
}

}  // namespace

VrResult RunVrCalibration(const Config& start, std::atomic<bool>& cancel,
                          const std::function<void(const std::string&)>& status, Preview* preview) {
    VrResult result;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSwapchain swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrActionSet actionSet = XR_NULL_HANDLE;

    // The correction layer must not be applied to the calibration app itself.
    SetEnvironmentVariableW(L"DISABLE_XR_APILAYER_NOVENDOR_virtual_prism", L"1");

    try {
        // ---- Instance ----
        const char* ext = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ici.applicationInfo.applicationName, "Virtual Prism Calibration");
        ici.applicationInfo.applicationVersion = 1;
        strcpy_s(ici.applicationInfo.engineName, "none");
        ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = &ext;
        XrResult r = xrCreateInstance(&ici, &instance);
        if (XR_FAILED(r))
            throw std::runtime_error("Could not start OpenXR. Is the Meta Quest Link app set as the active OpenXR runtime?");

        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId systemId;
        r = xrGetSystem(instance, &sgi, &systemId);
        if (XR_FAILED(r))
            throw std::runtime_error("No headset found. Start Meta Quest Link (connect the Quest) and try again.");

        // ---- D3D11 device on the adapter the runtime wants ----
        PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
        Check(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq),
              "xrGetInstanceProcAddr");
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        Check(getReq(instance, systemId, &req), "xrGetD3D11GraphicsRequirementsKHR");

        ComPtr<IDXGIFactory1> factory;
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; i++) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) != S_OK) break;
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (!memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID))) { adapter = a; break; }
        }
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        if (FAILED(D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                     nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                     &device, nullptr, &ctx)))
            throw std::runtime_error("Could not create a Direct3D 11 device.");

        // ---- Session ----
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &binding;
        sci.systemId = systemId;
        Check(xrCreateSession(instance, &sci, &session), "xrCreateSession");

        XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        rsci.poseInReferenceSpace.orientation.w = 1;
        Check(xrCreateReferenceSpace(session, &rsci, &viewSpace), "xrCreateReferenceSpace");

        // ---- Swapchains (one per eye) ----
        uint32_t fmtCount = 0;
        xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
        std::vector<int64_t> formats(fmtCount);
        xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, formats.data());
        const int64_t preferred[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                     DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
        int64_t format = 0;
        for (int64_t p : preferred)
            if (std::find(formats.begin(), formats.end(), p) != formats.end()) { format = p; break; }
        if (!format) throw std::runtime_error("No supported swapchain format.");
        bool swapRB = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_UNORM;

        std::vector<XrSwapchainImageD3D11KHR> images[2];
        ComPtr<ID3D11Texture2D> source[2];
        for (int e = 0; e < 2; e++) {
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = format;
            sc.sampleCount = 1;
            sc.width = sc.height = kImageSize;
            sc.faceCount = sc.arraySize = sc.mipCount = 1;
            Check(xrCreateSwapchain(session, &sc, &swapchains[e]), "xrCreateSwapchain");
            uint32_t n = 0;
            xrEnumerateSwapchainImages(swapchains[e], 0, &n, nullptr);
            images[e].assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            xrEnumerateSwapchainImages(swapchains[e], n, &n, (XrSwapchainImageBaseHeader*)images[e].data());

            D3D11_TEXTURE2D_DESC td = {};
            td.Width = td.Height = kImageSize;
            td.MipLevels = td.ArraySize = 1;
            td.Format = (DXGI_FORMAT)format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &source[e])))
                throw std::runtime_error("Could not create source texture.");
        }

        // ---- Input ----
        XrPath hand[2];
        xrStringToPath(instance, "/user/hand/left", &hand[0]);
        xrStringToPath(instance, "/user/hand/right", &hand[1]);
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(asci.actionSetName, "calibration");
        strcpy_s(asci.localizedActionSetName, "Calibration");
        Check(xrCreateActionSet(instance, &asci, &actionSet), "xrCreateActionSet");

        auto makeAction = [&](const char* name, XrActionType type) {
            XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
            ai.actionType = type;
            strcpy_s(ai.actionName, name);
            strcpy_s(ai.localizedActionName, name);
            ai.countSubactionPaths = 2;
            ai.subactionPaths = hand;
            XrAction a;
            Check(xrCreateAction(actionSet, &ai, &a), "xrCreateAction");
            return a;
        };
        XrAction aStick = makeAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        XrAction aClick = makeAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
        XrAction aCw = makeAction("rotate_cw", XR_ACTION_TYPE_BOOLEAN_INPUT);
        XrAction aCcw = makeAction("rotate_ccw", XR_ACTION_TYPE_BOOLEAN_INPUT);
        XrAction aTrigger = makeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
        XrAction aSqueeze = makeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
        XrAction aMenu = makeAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);

        std::vector<XrActionSuggestedBinding> bindings;
        auto bind = [&](XrAction a, const char* path) {
            XrPath p;
            xrStringToPath(instance, path, &p);
            bindings.push_back({a, p});
        };
        for (const char* h : {"left", "right"}) {
            std::string b = std::string("/user/hand/") + h + "/input/";
            bind(aStick, (b + "thumbstick").c_str());
            bind(aClick, (b + "thumbstick/click").c_str());
            bind(aTrigger, (b + "trigger/value").c_str());
            bind(aSqueeze, (b + "squeeze/value").c_str());
        }
        bind(aCw, "/user/hand/right/input/a/click");
        bind(aCcw, "/user/hand/right/input/b/click");
        bind(aCw, "/user/hand/left/input/y/click");
        bind(aCcw, "/user/hand/left/input/x/click");
        bind(aMenu, "/user/hand/left/input/menu/click");
        XrPath touch;
        xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch);
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = touch;
        sb.suggestedBindings = bindings.data();
        sb.countSuggestedBindings = (uint32_t)bindings.size();
        Check(xrSuggestInteractionProfileBindings(instance, &sb), "xrSuggestInteractionProfileBindings");
        XrSessionActionSetsAttachInfo att{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        att.countActionSets = 1;
        att.actionSets = &actionSet;
        Check(xrAttachSessionActionSets(session, &att), "xrAttachSessionActionSets");

        auto getBool = [&](XrAction a, int h) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a; gi.subactionPath = hand[h];
            XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
            xrGetActionStateBoolean(session, &gi, &s);
            return s.isActive && s.currentState;
        };
        auto getFloat = [&](XrAction a, int h) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a; gi.subactionPath = hand[h];
            XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
            xrGetActionStateFloat(session, &gi, &s);
            return s.isActive ? s.currentState : 0.0f;
        };
        auto getVec = [&](XrAction a, int h) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a; gi.subactionPath = hand[h];
            XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
            xrGetActionStateVector2f(session, &gi, &s);
            return s.isActive ? s.currentState : XrVector2f{0, 0};
        };

        // ---- State ----
        UiState ui;
        ui.values = start;
        AsyncRenderer renderer(swapRB, preview);
        if (preview) { std::lock_guard<std::mutex> l(preview->m); preview->active = true; preview->hasImage = false; }
        std::vector<int> drawnSig = Signature(ui);
        renderer.Request(ui);
        bool haveImage = false;
        auto lastRender = std::chrono::steady_clock::now();

        const float prismRate[3] = {2.0f, 0.5f, 0.1f};  // prism diopters per second at full stick
        const float rotRate[3] = {3.0f, 0.8f, 0.15f};   // degrees per second
        bool prevClick[2] = {}, prevSqueeze[2] = {}, prevMenu = false, prevLTrig = false;
        float saveHold = 0;
        XrTime lastTime = 0;

        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        bool running = false, exiting = false, exitRequested = false;
        status("Waiting for the headset... put it on.");

        while (!exiting) {
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
            while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
                if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    state = ((XrEventDataSessionStateChanged*)&ev)->state;
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        Check(xrBeginSession(session, &bi), "xrBeginSession");
                        running = true;
                        status("VR calibration running. Follow the instructions in the headset.");
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        xrEndSession(session);
                        running = false;
                        exiting = true;
                    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                        exiting = true;
                    }
                } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                    exiting = true;
                }
                ev = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            if (exiting) break;
            if (!running) { Sleep(10); continue; }

            if (cancel && !exitRequested) { xrRequestExitSession(session); exitRequested = true; }

            XrFrameState fs{XR_TYPE_FRAME_STATE};
            Check(xrWaitFrame(session, nullptr, &fs), "xrWaitFrame");
            Check(xrBeginFrame(session, nullptr), "xrBeginFrame");

            std::vector<const XrCompositionLayerBaseHeader*> layers;
            XrCompositionLayerQuad quads[2];

            if (fs.shouldRender) {
                float dt = lastTime ? (float)std::clamp((fs.predictedDisplayTime - lastTime) * 1e-9, 0.0, 0.1) : 0.0f;
                lastTime = fs.predictedDisplayTime;

                // -- Input --
                if (state == XR_SESSION_STATE_FOCUSED && !exitRequested) {
                    XrActiveActionSet active{actionSet, XR_NULL_PATH};
                    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
                    si.countActiveActionSets = 1;
                    si.activeActionSets = &active;
                    xrSyncActions(session, &si);
                    for (int h = 0; h < 2; h++) {
                        EyeCorrection& e = ui.values.eye[h];
                        XrVector2f st = getVec(aStick, h);
                        e.hPrism += Dead(st.x) * prismRate[ui.stepMode] * dt;
                        e.vPrism += Dead(st.y) * prismRate[ui.stepMode] * dt;
                        if (getBool(aCw, h)) e.rollDeg += rotRate[ui.stepMode] * dt;
                        if (getBool(aCcw, h)) e.rollDeg -= rotRate[ui.stepMode] * dt;
                        e.hPrism = std::clamp(e.hPrism, -30.0f, 30.0f);
                        e.vPrism = std::clamp(e.vPrism, -30.0f, 30.0f);
                        e.rollDeg = std::clamp(e.rollDeg, -15.0f, 15.0f);
                        bool click = getBool(aClick, h);
                        if (click && !prevClick[h]) e = EyeCorrection();
                        prevClick[h] = click;
                        bool sq = getFloat(aSqueeze, h) > 0.7f;
                        if (sq && !prevSqueeze[h]) {
                            if (h == 0) ui.scene = (ui.scene + 1) % SceneCount();
                            else ui.stepMode = (ui.stepMode + 1) % 3;
                        }
                        prevSqueeze[h] = sq;
                    }
                    bool menu = getBool(aMenu, 0);
                    if (menu && !prevMenu) ui.help = !ui.help;
                    prevMenu = menu;
                    bool lt = getFloat(aTrigger, 0) > 0.7f;
                    if (lt && !prevLTrig) ui.eyeMode = (ui.eyeMode + 1) % 3;
                    prevLTrig = lt;

                    if (getFloat(aTrigger, 1) > 0.9f) saveHold += dt; else saveHold = 0;
                    ui.saveProgress = std::min(saveHold / 0.6f, 1.0f);
                    if (saveHold >= 0.6f) {
                        result.saved = true;
                        result.config = ui.values;
                        xrRequestExitSession(session);
                        exitRequested = true;
                    }
                }

                if (preview) {
                    std::lock_guard<std::mutex> l(preview->m);
                    preview->values = ui.values;
                    preview->eyeMode = ui.eyeMode;
                }

                // -- Re-render the images when something changed (throttled, off-thread) --
                auto now = std::chrono::steady_clock::now();
                if (!renderer.Busy() && now - lastRender > std::chrono::milliseconds(80)) {
                    std::vector<int> sig = Signature(ui);
                    if (sig != drawnSig) {
                        drawnSig = sig;
                        lastRender = now;
                        renderer.Request(ui);
                    }
                }
                if (renderer.TakeReady([&](int e, const uint32_t* px) {
                        ctx->UpdateSubresource(source[e].Get(), 0, nullptr, px, kImageSize * 4, 0);
                    }))
                    haveImage = true;

                // -- Place one head-locked quad per eye, shifted by the eye's correction --
                XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
                vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                vli.displayTime = fs.predictedDisplayTime;
                vli.space = viewSpace;
                XrViewState vs{XR_TYPE_VIEW_STATE};
                XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t vcount = 0;
                if (haveImage && XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, 2, &vcount, views)) && vcount == 2) {
                    for (int e = 0; e < 2; e++) {
                        if (ui.eyeMode != 0 && ui.eyeMode != e + 1) continue;
                        uint32_t idx;
                        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        Check(xrAcquireSwapchainImage(swapchains[e], &ai, &idx), "xrAcquireSwapchainImage");
                        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wi.timeout = XR_INFINITE_DURATION;
                        Check(xrWaitSwapchainImage(swapchains[e], &wi), "xrWaitSwapchainImage");
                        ctx->CopyResource(images[e][idx].texture, source[e].Get());
                        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        Check(xrReleaseSwapchainImage(swapchains[e], &ri), "xrReleaseSwapchainImage");

                        Quat rot = ShiftRotation(ui.values.eye[e]);
                        const float fwd[3] = {0, 0, -kQuadDistance};
                        float off[3];
                        Rotate(rot, fwd, off);
                        XrCompositionLayerQuad& q = quads[e];
                        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                        q.space = viewSpace;
                        q.eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                        q.subImage.swapchain = swapchains[e];
                        q.subImage.imageRect = {{0, 0}, {kImageSize, kImageSize}};
                        q.pose.orientation = {rot.x, rot.y, rot.z, rot.w};
                        q.pose.position = {views[e].pose.position.x + off[0], views[e].pose.position.y + off[1],
                                           views[e].pose.position.z + off[2]};
                        q.size = {2 * kQuadHalfTan * kQuadDistance, 2 * kQuadHalfTan * kQuadDistance};
                        layers.push_back((const XrCompositionLayerBaseHeader*)&q);
                    }
                    ctx->Flush();
                }
            }

            XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = (uint32_t)layers.size();
            fei.layers = layers.data();
            Check(xrEndFrame(session, &fei), "xrEndFrame");
        }

        result.message = result.saved ? "Calibration saved." : "Calibration closed without saving.";
    } catch (const std::exception& e) {
        result.message = e.what();
        result.saved = false;
    }

    if (preview) { std::lock_guard<std::mutex> l(preview->m); preview->active = false; }
    for (auto& s : swapchains) if (s) xrDestroySwapchain(s);
    if (actionSet) xrDestroyActionSet(actionSet);
    if (viewSpace) xrDestroySpace(viewSpace);
    if (session) xrDestroySession(session);
    if (instance) xrDestroyInstance(instance);
    return result;
}

}  // namespace dfx
