// CPU rendering (GDI+) of the per-eye calibration images shown in VR.
#pragma once
#include <cstdint>

#include "shared.h"

namespace dfx {

constexpr int kImageSize = 2048;       // per-eye texture is kImageSize x kImageSize
constexpr float kQuadDistance = 2.0f;  // meters
constexpr float kQuadHalfTan = 0.9f;   // quad half-size as tan(angle): about +/-42 degrees

// Scene index of the 3D test room (drawn by the VR loop; the 2D image then only carries the HUD).
constexpr int kRoomScene = 6;

struct UiState {
    int scene = 0;
    int stepMode = 1;  // 0 coarse, 1 medium, 2 fine
    int eyeMode = 0;   // 0 both, 1 left only, 2 right only
    bool help = true;
    float saveProgress = 0;  // 0..1 while the save trigger is held
    Config values;
};

int SceneCount();
const wchar_t* SceneName(int scene);
const wchar_t* StepName(int step);

bool InitScenes();
void ShutdownScenes();

// Fills `bgra` (kImageSize^2 pixels, premultiplied BGRA) with the image for `eye` (0 left, 1 right).
void RenderEyeImage(int eye, const UiState& ui, uint32_t* bgra);

}  // namespace dfx
