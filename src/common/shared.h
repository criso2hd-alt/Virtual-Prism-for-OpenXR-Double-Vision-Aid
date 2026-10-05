// Shared between the OpenXR API layer and the settings/calibration app:
// config file handling and the "virtual prism" math.
#pragma once
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dfx {

constexpr double kPi = 3.14159265358979323846;

// Correction for one eye. Signs are from the wearer's point of view:
//   hPrism  > 0 : image moves right      (prism diopters, 1 = 1 cm at 1 m)
//   vPrism  > 0 : image moves up
//   rollDeg > 0 : image rotates clockwise
struct EyeCorrection {
    float hPrism = 0, vPrism = 0, rollDeg = 0;
    bool IsZero() const { return hPrism == 0 && vPrism == 0 && rollDeg == 0; }
};

struct Config {
    bool enabled = true;
    EyeCorrection eye[2];  // 0 = left, 1 = right
};

inline std::wstring ConfigDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = n ? std::wstring(buf, n) : L".";
    return dir + L"\\VirtualPrismOpenXR";
}
inline std::wstring ConfigPath() { return ConfigDir() + L"\\config.ini"; }

inline float ReadFloat(const wchar_t* section, const wchar_t* key, const std::wstring& path) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(section, key, L"0", buf, 64, path.c_str());
    return (float)_wtof(buf);
}
inline void WriteFloat(const wchar_t* section, const wchar_t* key, float v, const std::wstring& path) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.4f", v);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}

inline Config LoadConfig() {
    Config c;
    std::wstring p = ConfigPath();
    c.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, p.c_str()) != 0;
    const wchar_t* names[2] = {L"LeftEye", L"RightEye"};
    for (int i = 0; i < 2; i++) {
        c.eye[i].hPrism = ReadFloat(names[i], L"HorizontalPrism", p);
        c.eye[i].vPrism = ReadFloat(names[i], L"VerticalPrism", p);
        c.eye[i].rollDeg = ReadFloat(names[i], L"RotationDeg", p);
    }
    return c;
}

inline bool SaveConfig(const Config& c) {
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    std::wstring p = ConfigPath();
    WritePrivateProfileStringW(L"General", L"Enabled", c.enabled ? L"1" : L"0", p.c_str());
    const wchar_t* names[2] = {L"LeftEye", L"RightEye"};
    for (int i = 0; i < 2; i++) {
        WriteFloat(names[i], L"HorizontalPrism", c.eye[i].hPrism, p);
        WriteFloat(names[i], L"VerticalPrism", c.eye[i].vPrism, p);
        WriteFloat(names[i], L"RotationDeg", c.eye[i].rollDeg, p);
    }
    return true;
}

// ---- Quaternion helpers (x, y, z, w; Hamilton product) ----
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
inline Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat AxisAngle(float ax, float ay, float az, double rad) {
    float s = (float)std::sin(rad * 0.5), c = (float)std::cos(rad * 0.5);
    return {ax * s, ay * s, az * s, c};
}
inline void Rotate(const Quat& q, const float v[3], float out[3]) {
    // v' = q * (v,0) * q^-1
    Quat p = {v[0], v[1], v[2], 0};
    Quat r = Mul(Mul(q, p), Conj(q));
    out[0] = r.x; out[1] = r.y; out[2] = r.z;
}

inline double PrismToRadians(double prismDiopters) { return std::atan(prismDiopters / 100.0); }

// Rotation, in the eye's own frame (x right, y up, -z forward), that moves the
// whole image by the requested correction: what the eye sees at direction d
// ends up at direction R * d.
inline Quat ShiftRotation(const EyeCorrection& c) {
    Quat yaw = AxisAngle(0, 1, 0, -PrismToRadians(c.hPrism));
    Quat pitch = AxisAngle(1, 0, 0, PrismToRadians(c.vPrism));
    Quat roll = AxisAngle(0, 0, 1, -c.rollDeg * kPi / 180.0);
    return Mul(Mul(yaw, pitch), roll);
}

}  // namespace dfx
