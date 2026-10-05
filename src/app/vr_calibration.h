// Runs the in-headset calibration session (blocking until the user saves/exits).
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "shared.h"

namespace dfx {

// Live copy of what the wearer sees, for the desktop spectator view.
constexpr int kPreviewSize = 512;
struct Preview {
    std::mutex m;
    bool active = false;
    bool hasImage = false;
    std::vector<uint32_t> img[2];  // kPreviewSize^2 BGRA, unshifted per-eye images
    Config values;
    int eyeMode = 0;  // 0 both, 1 left only, 2 right only
};

struct VrResult {
    bool saved = false;  // true if the user saved with the right trigger
    Config config;       // final values (valid when saved)
    std::string message; // human-readable outcome / error
};

VrResult RunVrCalibration(const Config& start, std::atomic<bool>& cancel,
                          const std::function<void(const std::string&)>& status, Preview* preview = nullptr);

}  // namespace dfx
