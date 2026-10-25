#include "gpu/d3d11/post_effects.h"
#include "kernel/kernel.h"
#include "host/settings.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>

namespace tz::gpu {
namespace {

template <typename T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

// Layout of cbuffer PostFx below.
struct PostConstants {
    float ndcToViewMul[2], ndcToViewAdd[2];
    float projA, projB, depthScale, depthOffset;
    float texel[2], radius, projScale;
    float falloffMul, falloffAdd, strength, maxDistance;
    float sharpness;
    uint32_t debugView;
    int32_t blurStep[2];
};
static_assert(sizeof(PostConstants) == 80, "PostFx layout");

// Occlusion fades out toward this view depth (game units, about centimeters): fog
// covers distant surfaces, and their occlusion radius is a pixel or less anyway.
constexpr float kMaxDistance = 8000.0f;

// All passes draw one full-screen triangle. View space is the projection's: x right,
// y up, z (= clip w) forward, reconstructed from uv (y down) and the view depth.
const char* kPostShader = R"(
cbuffer PostFx : register(b0) {
    float2 ndcToViewMul;   // view xy at z = 1: uv * ndcToViewMul + ndcToViewAdd
    float2 ndcToViewAdd;
    float projA, projB;    // z_ndc = projA + projB / z
    float depthScale, depthOffset;  // stored depth = depthScale * z_ndc + depthOffset
    float2 texel;          // 1 / size of the target being drawn
    float radius;          // occlusion radius in game units
    float projScale;       // half-res pixels per game unit at z = 1
    float falloffMul, falloffAdd;  // occluder weight over the outer part of the radius
    float strength;        // occlusion exponent; 0 = no occlusion
    float maxDistance;     // occlusion fades out toward this depth
    float sharpness;       // CAS sharpness 0-1; negative = no sharpening
    uint debugView;        // 1: the occlusion alone (TENCHU_TRACE=aoview)
    int2 blurStep;         // (1, 0) or (0, 1)
};
SamplerState pointClamp : register(s0);
static const float PI = 3.14159265, HALF_PI = 1.57079633;

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Inverse of Float20e4AsUnorm (xenos_backend.cpp): the 24 bits are a 20e4 float.
float DecodeDepth(float stored) {
    uint bits = uint(stored * 16777215.0 + 0.5), e = bits >> 20, m = bits & 0xFFFFFu;
    return e == 0 ? float(m) * exp2(-34.0) : (1.0 + float(m) * exp2(-20.0)) * exp2(float(e) - 15.0);
}
float ViewZ(float stored) {
    float zNdc = (DecodeDepth(stored) - depthOffset) / depthScale;
    return zNdc >= 1.0 ? 1e9 : projB / (zNdc - projA);  // the far plane and the cleared sky
}
float3 ViewPosition(float2 uv, float z) { return float3((uv * ndcToViewMul + ndcToViewAdd) * z, z); }

// ---- 1. View depth at half resolution, at the center of each 2x2 block: on a continuous
// surface the harmonic mean (1/z is linear on planes, so flat surfaces stay flat and do
// not occlude themselves), across an edge the nearest depth.
Texture2D<float> depthIn : register(t0);
float prepass(float4 position : SV_Position) : SV_Target {
    uint width, height;
    depthIn.GetDimensions(width, height);
    int2 p = int2(position.xy) * 2, last = int2(width, height) - 1;
    float4 z = float4(ViewZ(depthIn.Load(int3(min(p, last), 0))), ViewZ(depthIn.Load(int3(min(p + int2(1, 0), last), 0))),
                      ViewZ(depthIn.Load(int3(min(p + int2(0, 1), last), 0))), ViewZ(depthIn.Load(int3(min(p + 1, last), 0))));
    float nearest = min(min(z.x, z.y), min(z.z, z.w)), farthest = max(max(z.x, z.y), max(z.z, z.w));
    return farthest - nearest < 0.05 * nearest ? 4.0 / dot(1.0 / z, 1.0) : nearest;
}

// ---- 2. Ground-truth ambient occlusion (Jimenez et al. 2016), simplified from XeGTAO:
// in each slice through the view vector, the highest horizon on either side bounds the
// visible arc, which is integrated against the normal's cosine. Output: visibility
// (1 = open) and the view depth for the depth-aware passes.
#define SLICES 3
#define STEPS 4
Texture2D<float> viewDepth : register(t0);
float FastAcos(float x) {
    float a = min(abs(x), 1.0);  // rounding can leave dot products of unit vectors just above 1
    float r = (-0.156583 * a + HALF_PI) * sqrt(1.0 - a);
    return x >= 0 ? r : PI - r;
}
// Interleaved gradient noise (Jimenez): an even per-pixel rotation the blur averages out.
float IGN(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
float3 SamplePosition(float2 uv) { return ViewPosition(uv, viewDepth.SampleLevel(pointClamp, uv, 0)); }
float2 gtao(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy * texel;
    float3 P = SamplePosition(uv);
    if (P.z >= maxDistance) return float2(1.0, min(P.z, 65000.0));
    // The normal from the neighbors on the side with the smaller depth step (no normals
    // across silhouettes).
    float3 l = SamplePosition(uv - float2(texel.x, 0)), r = SamplePosition(uv + float2(texel.x, 0));
    float3 t = SamplePosition(uv - float2(0, texel.y)), b = SamplePosition(uv + float2(0, texel.y));
    float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
    float3 dy = abs(b.z - P.z) < abs(P.z - t.z) ? b - P : P - t;
    float3 V = normalize(-P), normal = cross(dx, dy);
    float3 N = dot(normal, normal) > 0.0 ? normalize(normal) : V;
    if (dot(N, V) < 0.0) N = -N;
    float screenRadius = radius * projScale / P.z;  // half-res pixels
    if (screenRadius < 1.0) return float2(1.0, P.z);
    float minStep = 1.3 / screenRadius;             // the nearest samples stay off this pixel
    float noiseSlice = IGN(position.xy), noiseStep = IGN(position.xy + float2(17.0, 59.0));
    float visibility = 0.0;
    [unroll] for (int s = 0; s < SLICES; ++s) {
        float phi = (s + noiseSlice) * (PI / SLICES);
        float3 direction = float3(cos(phi), sin(phi), 0.0);
        float2 omega = float2(direction.x, -direction.y) * screenRadius;  // pixels; screen y points down
        float3 ortho = direction - dot(direction, V) * V;
        float3 axis = normalize(cross(ortho, V));
        float3 projectedN = N - axis * dot(N, axis);
        float projectedLength = length(projectedN) + 1e-5;
        float cosN = saturate(dot(projectedN, V) / projectedLength);
        float n = sign(dot(ortho, projectedN)) * FastAcos(cosN);
        float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
        float horizon0 = low0, horizon1 = low1;
        [unroll] for (int j = 0; j < STEPS; ++j) {
            float k = (j + frac(noiseStep + (s + j * SLICES) * 0.618034)) / STEPS;
            // Denser near the pixel; whole pixels, so samples hit texel centers.
            float2 offset = round(omega * (k * k + minStep)) * texel;
            float3 d0 = SamplePosition(uv + offset) - P, d1 = SamplePosition(uv - offset) - P;
            float l0 = max(length(d0), 1e-4), l1 = max(length(d1), 1e-4);
            horizon0 = max(horizon0, lerp(low0, dot(d0 / l0, V), saturate(l0 * falloffMul + falloffAdd)));
            horizon1 = max(horizon1, lerp(low1, dot(d1 / l1, V), saturate(l1 * falloffMul + falloffAdd)));
        }
        float h0 = n + clamp(-FastAcos(horizon1) - n, -HALF_PI, HALF_PI);
        float h1 = n + clamp(FastAcos(horizon0) - n, -HALF_PI, HALF_PI);
        float sinN = sin(n);
        visibility += lerp(projectedLength, 1.0, 0.05) *
                      (2.0 * cosN + 2.0 * (h0 + h1) * sinN - cos(2.0 * h0 - n) - cos(2.0 * h1 - n)) * 0.25;
    }
    visibility = isnan(visibility) ? 1.0 : saturate(visibility / SLICES);  // never a black pixel
    // Fades in over the first pixels of radius (distant surfaces).
    return float2(lerp(1.0, visibility, saturate(screenRadius - 1.0)), P.z);
}

// ---- 3. Depth-aware blur of the occlusion along one axis (texels outside the target load
// as depth 0 and get no weight).
Texture2D<float2> occlusionIn : register(t0);
float2 blur(float4 position : SV_Position) : SV_Target {
    int2 p = int2(position.xy);
    float2 center = occlusionIn.Load(int3(p, 0));
    float sum = center.x, weights = 1.0;
    [unroll] for (int i = -3; i <= 3; ++i) {
        if (i == 0) continue;
        float2 s = occlusionIn.Load(int3(p + blurStep * i, 0));
        float w = exp2(-0.15 * i * i) * saturate(1.0 - abs(s.y - center.y) / (0.05 * abs(i) * center.y));
        sum += s.x * w;
        weights += w;
    }
    return float2(sum / weights, center.y);
}

// ---- 4. Full resolution: sharpening, then the occlusion, upsampled by depth.
Texture2D scene : register(t0);
Texture2D<float> depthFull : register(t1);
Texture2D<float2> occlusion : register(t2);
float3 SceneAt(int2 p, int2 last) { return scene.Load(int3(clamp(p, 0, last), 0)).rgb; }
// AMD FidelityFX CAS (MIT), without scaling: a 5-tap sharpen whose strength follows the
// local contrast (soft minimum and maximum of the 3x3 neighborhood), so edges do not ring.
float3 Cas(int2 p, int2 last) {
    float3 a = SceneAt(p + int2(-1, -1), last), b = SceneAt(p + int2(0, -1), last), c = SceneAt(p + int2(1, -1), last);
    float3 d = SceneAt(p + int2(-1, 0), last), e = SceneAt(p, last), f = SceneAt(p + int2(1, 0), last);
    float3 g = SceneAt(p + int2(-1, 1), last), h = SceneAt(p + int2(0, 1), last), i = SceneAt(p + int2(1, 1), last);
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, i)));
    float3 amplitude = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));
    float3 w = amplitude * (-1.0 / lerp(8.0, 5.0, sharpness));
    return saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
}
float4 apply(float4 position : SV_Position) : SV_Target {
    uint width, height;
    scene.GetDimensions(width, height);
    int2 p = int2(position.xy), last = int2(width, height) - 1;
    float4 color = scene.Load(int3(p, 0));  // alpha stays exactly as resolved
    if (sharpness >= 0.0) color.rgb = Cas(p, last);
    if (strength > 0.0) {
        float z = ViewZ(depthFull.Load(int3(p, 0)));
        float2 uv = position.xy * texel;
        float4 open = occlusion.GatherRed(pointClamp, uv), depths = occlusion.GatherGreen(pointClamp, uv);
        float4 weights = 1.0 / (1e-3 + abs(depths - z) / z);
        float visibility = max(dot(open, weights) / dot(weights, 1.0), 0.03);
        float fade = saturate((maxDistance - z) / (0.25 * maxDistance));                 // fog, far distance
        float protect = 1.0 - smoothstep(0.7, 1.0, max(color.r, max(color.g, color.b)));  // lamps, fire, sky
        // The occlusion is linear light, the scene gamma 2.2.
        float shade = pow(visibility, strength * fade * protect / 2.2);
        color.rgb = debugView ? shade.xxx : color.rgb * shade;
    }
    return color;
}
)";

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length3(const float a[3]) { return std::sqrt(dot3(a, a)); }

// M = World * View * Projection for row vectors (position * M). For a perspective
// P = [[px,0,0,0],[0,py,0,0],[0,0,a,1],[0,0,b,0]] the w column (rows 0-2) is WV's view axis,
// the z column is a times it, and M[3][2] = a * M[3][3] + b; x and y are perpendicular
// to w with lengths px and py times its length. World scale cancels out when uniform.
bool extractProjection(const float m[4][4], float zScale, float zOffset, Projection& out) {
    const float x[3] = {m[0][0], m[1][0], m[2][0]}, y[3] = {m[0][1], m[1][1], m[2][1]};
    const float z[3] = {m[0][2], m[1][2], m[2][2]}, w[3] = {m[0][3], m[1][3], m[2][3]};
    const float ww = dot3(w, w);
    if (!(ww > 1e-12f) || !std::isfinite(ww)) return false;
    const float scale = std::sqrt(ww), a = dot3(z, w) / ww;
    const float zw[3] = {z[1] * w[2] - z[2] * w[1], z[2] * w[0] - z[0] * w[2], z[0] * w[1] - z[1] * w[0]};
    if (length3(zw) > 1e-3f * length3(z) * scale) return false;  // not a perspective projection
    if (std::fabs(dot3(x, w)) > 1e-3f * length3(x) * scale || std::fabs(dot3(y, w)) > 1e-3f * length3(y) * scale)
        return false;  // off-center or skewed
    const float b = m[3][2] - a * m[3][3];
    const float px = length3(x) / scale, py = length3(y) / scale;
    // The main camera: 16:9, near/far below 1/10, a positive near plane.
    if (!(px > 0.0f) || std::fabs(py / px - 16.0f / 9.0f) > 0.02f || a < 0.9999f || a > 1.1f || !(b < 0.0f))
        return false;
    out = {px, py, a, b, zScale, zOffset};
    return true;
}

bool sameProjection(const Projection& p, const Projection& q) {
    auto close = [](float u, float v) { return std::fabs(u - v) <= 1e-3f * (std::max)(std::fabs(u), std::fabs(v)); };
    return close(p.px, q.px) && close(p.py, q.py) && std::fabs(p.a - q.a) <= 1e-5f && close(p.b, q.b) &&
           p.zScale == q.zScale && p.zOffset == q.zOffset;
}

}  // namespace

void PostEffects::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    context_ = context;
    auto compile = [&](const char* entry, const char* profile) -> ID3DBlob* {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(D3DCompile(kPostShader, strlen(kPostShader), "postfx", nullptr, nullptr, entry, profile, 0, 0, &code,
                              &errors)))
            logf("[postfx] %s: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        release(errors);
        return code;
    };
    if (ID3DBlob* code = compile("vs", "vs_5_0")) {
        device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex_);
        release(code);
    }
    const std::pair<const char*, ID3D11PixelShader**> pixelShaders[] = {
        {"prepass", &prepass_}, {"gtao", &gtao_}, {"blur", &blur_}, {"apply", &apply_}};
    for (const auto& [entry, shader] : pixelShaders)
        if (ID3DBlob* code = compile(entry, "ps_5_0")) {
            device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader);
            release(code);
        }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(PostConstants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&buffer, nullptr, &constants_);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sampler, &pointClamp_);
    ready_ = vertex_ && prepass_ && gtao_ && blur_ && apply_ && constants_ && pointClamp_;
    if (!ready_) logf("[postfx] unavailable (shader or state creation failed)");
}

bool PostEffects::enabled() const {
    const host::Settings& s = host::settings();
    return ready_ && ((s.ambientOcclusion && s.aoStrength > 0) || s.sharpening > 0);
}

bool PostEffects::wantsProjection() const {
    static const bool trace = traceEnabled("postfx");
    return ready_ && (host::settings().ambientOcclusion || trace);
}

void PostEffects::noteProjection(const float* constants, float zScale, float zOffset) {
    // A few hundred depth-writing draws decide the vote; later ones only add cost.
    if (offered_ >= 256 || zScale == 0.0f) return;
    ++offered_;
    // The world-view-projection matrix is among the first constants: c0-c3 as rows in the
    // static mesh shaders (oPos = x*c0 + y*c1 + z*c2 + c3), c3-c6 as columns in the
    // terrain's (oPos.x = dp4(position, c3), ...). The checks in extractProjection keep
    // other data from passing.
    Projection projection;
    bool found = false;
    for (int base = 0; base < 8 && !found; ++base) {
        const float* c = constants + base * 4;
        float rows[4][4], columns[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                rows[i][j] = c[i * 4 + j];
                columns[i][j] = c[j * 4 + i];
            }
        found = extractProjection(rows, zScale, zOffset, projection) ||
                extractProjection(columns, zScale, zOffset, projection);
    }
    if (!found) return;
    for (uint32_t i = 0; i < candidateCount_; ++i)
        if (sameProjection(candidates_[i].projection, projection)) {
            ++candidates_[i].votes;
            return;
        }
    if (candidateCount_ < std::size(candidates_)) candidates_[candidateCount_++] = {projection, 1};
}

const Projection* PostEffects::projection() const {
    const Candidate* best = nullptr;
    for (uint32_t i = 0; i < candidateCount_; ++i)
        if (!best || candidates_[i].votes > best->votes) best = &candidates_[i];
    if (best) return &best->projection;
    return lastFrameValid_ ? &lastFrame_ : nullptr;
}

void PostEffects::skipped(const char* reason) {
    skipReason_ = reason;
    ++skippedCount_;
}

bool PostEffects::ensure(Target& target, UINT width, UINT height, DXGI_FORMAT format) {
    if (target.texture && target.width == width && target.height == height && target.format == format) return true;
    release(target.view);
    release(target.target);
    release(target.texture);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &target.texture))) return false;
    device_->CreateRenderTargetView(target.texture, nullptr, &target.target);
    device_->CreateShaderResourceView(target.texture, nullptr, &target.view);
    target.width = width;
    target.height = height;
    target.format = format;
    return target.target && target.view;
}

void PostEffects::writeConstants(const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, data, size);
    context_->Unmap(constants_, 0);
}

void PostEffects::pass(ID3D11PixelShader* shader, const Target& target, ID3D11ShaderResourceView* const* inputs,
                       UINT count) {
    context_->OMSetRenderTargets(1, &target.target, nullptr);
    const D3D11_VIEWPORT viewport{0, 0, float(target.width), float(target.height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->PSSetShader(shader, nullptr, 0);
    context_->PSSetShaderResources(0, count, inputs);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {};
    context_->PSSetShaderResources(0, count, none);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

ID3D11ShaderResourceView* PostEffects::processScene(ID3D11Texture2D* scene, ID3D11ShaderResourceView* sceneView,
                                                    ID3D11Texture2D* depth, ID3D11ShaderResourceView* depthView,
                                                    uint64_t generation) {
    const host::Settings& settings = host::settings();
    const bool occlusion = settings.ambientOcclusion && settings.aoStrength > 0, sharpen = settings.sharpening > 0;
    if (!ready_ || (!occlusion && !sharpen)) return nullptr;
    if (lastValid_ && sceneView == lastScene_ && depthView == lastDepth_ && generation == lastGeneration_) {
        ++applied_;
        return output_.view;
    }
    D3D11_TEXTURE2D_DESC sceneDesc{}, depthDesc{};
    scene->GetDesc(&sceneDesc);
    if (sceneDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || sceneDesc.SampleDesc.Count != 1) {
        skipped("the scene is not RGBA8");
        return nullptr;
    }
    // Ambient occlusion needs this frame's camera (or the last frame's) and a depth resolve
    // of the scene's size. Without them the frame gets sharpening only, or stays as it is.
    const Projection* camera = occlusion ? projection() : nullptr;
    bool withOcclusion = camera != nullptr;
    if (withOcclusion) {
        depth->GetDesc(&depthDesc);
        withOcclusion = depthDesc.Format == DXGI_FORMAT_R32_FLOAT && depthDesc.Width == sceneDesc.Width &&
                        depthDesc.Height == sceneDesc.Height;
    }
    if (occlusion && !withOcclusion) {
        skipped(!camera ? "no camera projection this frame" : "the depth resolve differs from the scene");
        if (!sharpen) return nullptr;
    }
    const UINT width = sceneDesc.Width, height = sceneDesc.Height;
    const UINT halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
    if (!ensure(output_, width, height, sceneDesc.Format) ||
        (withOcclusion && (!ensure(viewDepth_, halfWidth, halfHeight, DXGI_FORMAT_R32_FLOAT) ||
                           !ensure(occlusion_[0], halfWidth, halfHeight, DXGI_FORMAT_R16G16_FLOAT) ||
                           !ensure(occlusion_[1], halfWidth, halfHeight, DXGI_FORMAT_R16G16_FLOAT)))) {
        skipped("cannot create the effect targets");
        lastValid_ = false;
        return nullptr;
    }

    // Common state; the backend sets all of its own again afterwards (forgetBoundState).
    ID3D11ShaderResourceView* unbound[16] = {};
    context_->PSSetShaderResources(0, 16, unbound);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_, nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->RSSetState(nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->PSSetSamplers(0, 1, &pointClamp_);
    context_->PSSetConstantBuffers(0, 1, &constants_);

    PostConstants k{};
    static const bool aoView = traceEnabled("aoview");
    k.debugView = aoView ? 1 : 0;
    k.maxDistance = kMaxDistance;
    k.sharpness = sharpen ? float(settings.sharpening - 1) / 99.0f : -1.0f;
    if (withOcclusion) {
        const Projection& p = *camera;
        k.ndcToViewMul[0] = 2.0f / p.px;
        k.ndcToViewMul[1] = -2.0f / p.py;
        k.ndcToViewAdd[0] = -1.0f / p.px;
        k.ndcToViewAdd[1] = 1.0f / p.py;
        k.projA = p.a;
        k.projB = p.b;
        k.depthScale = p.zScale;
        k.depthOffset = p.zOffset;
        k.radius = float(settings.aoRadius);
        k.projScale = 0.5f * float(halfHeight) * p.py;
        // XeGTAO's falloff: full weight up to 38.5% of the radius, none at the radius.
        const float falloffRange = 0.615f * k.radius;
        k.falloffMul = -1.0f / falloffRange;
        k.falloffAdd = (k.radius - falloffRange) / falloffRange + 1.0f;
        k.strength = float(settings.aoStrength) / 100.0f;
        k.texel[0] = 1.0f / float(halfWidth);
        k.texel[1] = 1.0f / float(halfHeight);
        writeConstants(&k, sizeof(k));
        pass(prepass_, viewDepth_, &depthView, 1);
        pass(gtao_, occlusion_[0], &viewDepth_.view, 1);
        k.blurStep[0] = 1;
        writeConstants(&k, sizeof(k));
        pass(blur_, occlusion_[1], &occlusion_[0].view, 1);
        k.blurStep[0] = 0;
        k.blurStep[1] = 1;
        writeConstants(&k, sizeof(k));
        pass(blur_, occlusion_[0], &occlusion_[1].view, 1);
    }
    k.texel[0] = 1.0f / float(width);
    k.texel[1] = 1.0f / float(height);
    writeConstants(&k, sizeof(k));
    ID3D11ShaderResourceView* inputs[3] = {sceneView, withOcclusion ? depthView : nullptr,
                                           withOcclusion ? occlusion_[0].view : nullptr};
    pass(apply_, output_, inputs, 3);
    lastScene_ = sceneView;
    lastDepth_ = depthView;
    lastGeneration_ = generation;
    lastValid_ = true;
    ++applied_;
    return output_.view;
}

void PostEffects::endFrame() {
    logStatus();
    const Candidate* best = nullptr;
    for (uint32_t i = 0; i < candidateCount_; ++i)
        if (!best || candidates_[i].votes > best->votes) best = &candidates_[i];
    lastFrameValid_ = best != nullptr;
    if (best) lastFrame_ = best->projection;
    candidateCount_ = offered_ = 0;
    lastValid_ = false;
}

// TENCHU_TRACE=postfx, once a second: the camera the vote chose (expect aspect 1.778 and a
// steady field of view) and what happened to the composites since the last line.
void PostEffects::logStatus() {
    static const bool trace = traceEnabled("postfx");
    if (!trace) return;
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - statusAt_ < 1000) return;
    statusAt_ = now;
    uint32_t votes = 0, total = 0;
    const Candidate* best = nullptr;
    for (uint32_t i = 0; i < candidateCount_; ++i) {
        total += candidates_[i].votes;
        if (!best || candidates_[i].votes > best->votes) best = &candidates_[i];
    }
    if (best) {
        const Projection& p = best->projection;
        votes = best->votes;
        const float nearPlane = -p.b / p.a, farPlane = p.a > 1.0f ? p.b / (1.0f - p.a) : INFINITY;
        logf("[postfx] camera: fov %.1f deg, aspect %.3f, near %.2f, far %.0f, depth %+.0f*z%+.0f; "
             "%u of %u votes (%u candidates, %u draws offered)",
             2.0f * std::atan(1.0f / p.py) * 57.29578f, p.py / p.px, nearPlane, farPlane, p.zScale, p.zOffset, votes,
             total, candidateCount_, offered_);
    } else {
        logf("[postfx] camera: none this frame (%u depth-writing draws offered)", offered_);
    }
    logf("[postfx] composites: %u processed, %u left alone%s%s", applied_, skippedCount_,
         skipReason_ ? "; last reason: " : "", skipReason_ ? skipReason_ : "");
    applied_ = skippedCount_ = 0;
    skipReason_ = nullptr;
}

}  // namespace tz::gpu
