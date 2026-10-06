// A small 3D "test room" (near table, nested frames and posts at 0.7 - 12 m) drawn with D3D11.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

#include "shared.h"

namespace dfx {

// Row-vector convention (v * M), right-handed view space looking down -z, depth 0..1.
void BuildViewProj(const float pos[3], const Quat& orientation, float tanLeft, float tanRight, float tanUp,
                   float tanDown, float out[16]);

class RoomRenderer {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx);
    // Room space: the viewer starts at x=0,z=0 looking down -z. `floorY` / `eyeY` are heights in that space.
    void SetLayout(float floorY, float eyeY);
    void Draw(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const float viewProj[16],
              bool blank);

private:
    struct Box {
        float p[3], s[3], c[4];  // centre, size, colour (a > 0.5 = emissive, no lighting/grid)
    };
    void Add(float x, float y, float z, float sx, float sy, float sz, float r, float g, float b, float emissive = 0);

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_, ib_, cbFrame_, cbObj_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    std::vector<Box> boxes_;
};

}  // namespace dfx
