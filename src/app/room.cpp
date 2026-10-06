#include "room.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace dfx {

using Microsoft::WRL::ComPtr;

namespace {

const char* kShader = R"(
cbuffer Frame : register(b0) { row_major float4x4 viewProj; };
cbuffer Obj : register(b1) { float3 pos; float pad0; float3 scale; float pad1; float4 color; };
struct VSIn { float3 p : POSITION; float3 n : NORMAL; };
struct VSOut { float4 sv : SV_Position; float3 wp : TEXCOORD0; float3 n : NORMAL; };
VSOut VS(VSIn i) {
    VSOut o;
    float3 wp = pos + i.p * scale;
    o.wp = wp;
    o.sv = mul(float4(wp, 1), viewProj);
    o.n = i.n;
    return o;
}
float4 PS(VSOut i) : SV_Target {
    if (color.a > 0.5) return float4(color.rgb, 1);
    float3 n = normalize(i.n);
    float lam = saturate(dot(n, normalize(float3(0.4, 0.9, 0.3))));
    float3 c = color.rgb * (0.45 + 0.55 * lam);
    float2 uv = abs(n.x) > 0.5 ? i.wp.yz : (abs(n.y) > 0.5 ? i.wp.xz : i.wp.xy);
    uv *= 4.0;  // grid line every 25 cm, anti-aliased
    float2 w = max(fwidth(uv), 1e-4);
    float2 a = abs(frac(uv - 0.5) - 0.5) / w;
    float gl = 1.0 - min(min(a.x, a.y), 1.0);
    c = lerp(c, c * 0.3, gl * 0.8);
    return float4(c, 1);
}
)";

struct Vertex { float p[3], n[3]; };

bool Fail(ID3DBlob* err) {
    if (err) {
        OutputDebugStringA((const char*)err->GetBufferPointer());
        fprintf(stderr, "%s\n", (const char*)err->GetBufferPointer());
    }
    return false;
}

}  // namespace

void BuildViewProj(const float pos[3], const Quat& q, float tL, float tR, float tU, float tD, float out[16]) {
    const float nearZ = 0.05f, farZ = 100.0f;
    const float x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z[3] = {0, 0, 1};
    float ex[3], ey[3], ez[3];
    Rotate(q, x, ex);
    Rotate(q, y, ey);
    Rotate(q, z, ez);
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    // view = rows [ex.x ey.x ez.x 0; ex.y ey.y ez.y 0; ex.z ey.z ez.z 0; -p.ex -p.ey -p.ez 1]
    float V[16] = {ex[0], ey[0], ez[0], 0, ex[1], ey[1], ez[1], 0, ex[2], ey[2], ez[2], 0,
                   -dot(pos, ex), -dot(pos, ey), -dot(pos, ez), 1};
    float w = tR - tL, h = tU - tD;
    float P[16] = {2 / w, 0, 0, 0, 0, 2 / h, 0, 0, (tR + tL) / w, (tU + tD) / h, farZ / (nearZ - farZ), -1,
                   0, 0, nearZ * farZ / (nearZ - farZ), 0};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += V[r * 4 + k] * P[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

bool RoomRenderer::Init(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    dev_ = device;
    ctx_ = ctx;
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)))
        return Fail(err.Get());
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err)))
        return Fail(err.Get());
    device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    device->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);

    // Unit cube, 6 faces x 4 vertices
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    const float nrm[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; f++) {
        const float* n = nrm[f];
        int na = n[0] != 0 ? 0 : (n[1] != 0 ? 1 : 2);  // axis of the face normal
        int a = (na + 1) % 3, b = (na + 2) % 3;        // the two tangent axes
        float u[3] = {}, t[3] = {};
        u[a] = 1;
        t[b] = 1;
        uint16_t base = (uint16_t)v.size();
        for (int k = 0; k < 4; k++) {
            float su = (k & 1) ? 0.5f : -0.5f, st = (k & 2) ? 0.5f : -0.5f;
            Vertex vx;
            for (int i = 0; i < 3; i++) {
                vx.p[i] = n[i] * 0.5f + u[i] * su + t[i] * st;
                vx.n[i] = n[i];
            }
            v.push_back(vx);
        }
        for (uint16_t i : {0, 1, 2, 1, 3, 2}) idx.push_back(base + i);
    }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(v.size() * sizeof(Vertex));
    D3D11_SUBRESOURCE_DATA sd = {v.data()};
    device->CreateBuffer(&bd, &sd, &vb_);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(idx.size() * sizeof(uint16_t));
    sd.pSysMem = idx.data();
    device->CreateBuffer(&bd, &sd, &ib_);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 64;
    device->CreateBuffer(&bd, nullptr, &cbFrame_);
    bd.ByteWidth = 48;
    device->CreateBuffer(&bd, nullptr, &cbObj_);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &raster_);
    return vs_ && ps_ && layout_ && vb_ && ib_ && cbFrame_ && cbObj_ && raster_;
}

void RoomRenderer::Add(float x, float y, float z, float sx, float sy, float sz, float r, float g, float b,
                       float emissive) {
    boxes_.push_back({{x, y, z}, {sx, sy, sz}, {r, g, b, emissive}});
}

void RoomRenderer::SetLayout(float floorY, float eyeY) {
    boxes_.clear();
    const float H = 4.0f;
    // shell: floor, ceiling, back wall, side walls (corridor 10 m wide, 18 m long)
    Add(0, floorY - 0.05f, -6, 10, 0.1f, 24, 0.20f, 0.20f, 0.22f);
    Add(0, floorY + H, -6, 10, 0.1f, 24, 0.14f, 0.14f, 0.16f);
    Add(0, floorY + H / 2, -15, 10, H, 0.1f, 0.25f, 0.30f, 0.42f);
    Add(-5, floorY + H / 2, -6, 0.1f, H, 24, 0.40f, 0.26f, 0.22f);
    Add(5, floorY + H / 2, -6, 0.1f, H, 24, 0.22f, 0.30f, 0.40f);
    // pillars along the walls
    for (float d : {3.0f, 6.0f, 9.0f, 12.0f})
        for (float x : {-3.6f, 3.6f}) Add(x, floorY + H / 2, -d, 0.4f, H, 0.4f, 0.35f, 0.35f, 0.38f);

    struct Col { float r, g, b; };
    const Col cols[5] = {{1, 0.25f, 0.25f}, {1, 0.85f, 0.2f}, {0.3f, 1, 0.4f}, {0.2f, 0.9f, 1}, {1, 0.4f, 1}};
    const float dist[5] = {0.7f, 1.5f, 3.0f, 6.0f, 12.0f};
    for (int i = 0; i < 5; i++) {
        float d = dist[i], s = (0.30f - 0.052f * i) * d, t = 0.02f * d + 0.01f, z = -d;
        const Col& c = cols[i];
        // nested window frames around the line of sight: they overlap only when the correction is right at that distance
        Add(0, eyeY + s / 2, z, s + t, t, t, c.r, c.g, c.b, 1);
        Add(0, eyeY - s / 2, z, s + t, t, t, c.r, c.g, c.b, 1);
        Add(-s / 2, eyeY, z, t, s + t, t, c.r, c.g, c.b, 1);
        Add(s / 2, eyeY, z, t, s + t, t, c.r, c.g, c.b, 1);
        // floor stripe at the same distance
        Add(0, floorY + 0.006f, z, 4, 0.01f, 0.08f, c.r, c.g, c.b, 1);
    }
    // posts either side of the line of sight at several distances
    for (float d : {1.0f, 2.0f, 4.0f, 8.0f, 12.0f})
        for (float x : {-0.9f, 0.9f}) Add(x, floorY + 0.9f, -d, 0.1f, 1.8f, 0.1f, 0.85f, 0.85f, 0.9f);
    // near table with small objects (reading distance)
    float ty = floorY + 0.72f;
    Add(0, ty, -0.65f, 0.9f, 0.04f, 0.5f, 0.45f, 0.30f, 0.18f);
    for (float x : {-0.4f, 0.4f})
        for (float z : {-0.45f, -0.85f}) Add(x, floorY + 0.35f, z, 0.04f, 0.7f, 0.04f, 0.35f, 0.24f, 0.15f);
    Add(-0.25f, ty + 0.06f, -0.55f, 0.08f, 0.08f, 0.08f, 0.9f, 0.2f, 0.2f);
    Add(0.0f, ty + 0.06f, -0.68f, 0.08f, 0.08f, 0.08f, 0.2f, 0.4f, 0.9f);
    Add(0.25f, ty + 0.06f, -0.72f, 0.08f, 0.08f, 0.08f, 0.9f, 0.8f, 0.2f);
}

void RoomRenderer::Draw(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h,
                        const float viewProj[16], bool blank) {
    const float sky[4] = {0.01f, 0.015f, 0.03f, 1};
    ctx_->OMSetRenderTargets(1, &rtv, dsv);
    const float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv, blank ? black : sky);
    ctx_->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (blank) return;
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx_->IASetIndexBuffer(ib_.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {cbFrame_.Get(), cbObj_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ctx_->UpdateSubresource(cbFrame_.Get(), 0, nullptr, viewProj, 0, 0);
    for (const Box& b : boxes_) {
        float obj[12] = {b.p[0], b.p[1], b.p[2], 0, b.s[0], b.s[1], b.s[2], 0, b.c[0], b.c[1], b.c[2], b.c[3]};
        ctx_->UpdateSubresource(cbObj_.Get(), 0, nullptr, obj, 0, 0);
        ctx_->DrawIndexed(36, 0, 0);
    }
}

}  // namespace dfx
