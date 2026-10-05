// OpenXR API layer: applies a per-eye "virtual prism" (image shift + rotation).
//
// Trick: xrLocateViews returns each eye's pose rotated by the inverse of the
// wanted shift, so the game renders from a slightly turned viewpoint. In
// xrEndFrame the original pose is restored on the projection layers, so the
// compositor shows that image shifted by exactly the wanted amount.
#include <windows.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include "shared.h"

using namespace dfx;

namespace {

PFN_xrGetInstanceProcAddr g_nextGipa = nullptr;
PFN_xrLocateViews g_nextLocateViews = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;

std::mutex g_mutex;
Config g_config;
ULONGLONG g_lastCheck = 0;
FILETIME g_lastWrite = {};
bool g_loaded = false;

// Rotations applied per frame (keyed by display time) so xrEndFrame can undo them.
struct Applied {
    XrTime time = 0;
    Quat shift[2];
    bool valid = false;
};
Applied g_ring[16];
int g_ringPos = 0;

void Log(const char* msg) {
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    std::wstring path = ConfigDir() + L"\\layer.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a") == 0 && f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        fprintf(f, "%02d:%02d:%02d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, exe, msg);
        fclose(f);
    }
}

FILETIME ConfigWriteTime() {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    GetFileAttributesExW(ConfigPath().c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

Config CurrentConfig() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    if (g_lastCheck == 0 || now - g_lastCheck > 500) {
        g_lastCheck = now;
        FILETIME w = ConfigWriteTime();
        if (!g_loaded || g_lastWrite.dwLowDateTime != w.dwLowDateTime ||
            g_lastWrite.dwHighDateTime != w.dwHighDateTime) {
            g_loaded = true;
            g_lastWrite = w;
            g_config = LoadConfig();
        }
    }
    return g_config;
}

XrQuaternionf ToXr(const Quat& q) { return {q.x, q.y, q.z, q.w}; }
Quat FromXr(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }

XrResult XRAPI_PTR Layer_xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* state,
                                       uint32_t capacity, uint32_t* countOut, XrView* views) {
    XrResult r = g_nextLocateViews(session, info, state, capacity, countOut, views);
    if (XR_FAILED(r) || !views || !countOut || *countOut < 2 || capacity < 2) return r;
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return r;

    Config c = CurrentConfig();
    if (!c.enabled || (c.eye[0].IsZero() && c.eye[1].IsZero())) return r;

    Applied a;
    a.time = info->displayTime;
    a.valid = true;
    for (int i = 0; i < 2; i++) {
        a.shift[i] = ShiftRotation(c.eye[i]);
        views[i].pose.orientation = ToXr(Mul(FromXr(views[i].pose.orientation), Conj(a.shift[i])));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& e : g_ring)
        if (e.valid && e.time == a.time) { e = a; return r; }
    g_ring[g_ringPos++ & 15] = a;
    return r;
}

bool FindApplied(XrTime t, Applied& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& e : g_ring)
        if (e.valid && e.time == t) { out = e; return true; }
    return false;
}

XrResult XRAPI_PTR Layer_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    Applied a;
    if (!info || info->layerCount == 0 || !FindApplied(info->displayTime, a))
        return g_nextEndFrame(session, info);

    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
    std::deque<XrCompositionLayerProjection> projections;
    std::deque<std::vector<XrCompositionLayerProjectionView>> viewSets;

    for (auto& l : layers) {
        if (l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        auto* src = reinterpret_cast<const XrCompositionLayerProjection*>(l);
        if (src->viewCount < 2) continue;
        viewSets.emplace_back(src->views, src->views + src->viewCount);
        auto& v = viewSets.back();
        for (int i = 0; i < 2; i++)
            v[i].pose.orientation = ToXr(Mul(FromXr(v[i].pose.orientation), a.shift[i]));
        projections.push_back(*src);
        projections.back().views = v.data();
        l = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections.back());
    }

    XrFrameEndInfo copy = *info;
    copy.layers = layers.data();
    return g_nextEndFrame(session, &copy);
}

XrResult XRAPI_PTR Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    XrResult r = g_nextGipa(instance, name, function);
    if (XR_FAILED(r) || !name || !function || !*function) return r;
    if (!strcmp(name, "xrLocateViews")) {
        g_nextLocateViews = reinterpret_cast<PFN_xrLocateViews>(*function);
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrLocateViews);
    } else if (!strcmp(name, "xrEndFrame")) {
        g_nextEndFrame = reinterpret_cast<PFN_xrEndFrame>(*function);
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrEndFrame);
    }
    return r;
}

XrResult XRAPI_PTR Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                  const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (!layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION || !layerInfo->nextInfo)
        return XR_ERROR_INITIALIZATION_FAILED;

    g_nextGipa = layerInfo->nextInfo->nextGetInstanceProcAddr;
    XrApiLayerCreateInfo next = *layerInfo;
    next.nextInfo = layerInfo->nextInfo->next;
    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
    if (XR_SUCCEEDED(r)) {
        char buf[256];
        sprintf_s(buf, "layer loaded (app: %s)", info->applicationInfo.applicationName);
        Log(buf);
    }
    return r;
}

}  // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* /*layerName*/, XrNegotiateApiLayerRequest* request) {
    if (!loaderInfo || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
    request->createApiLayerInstance = Layer_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
