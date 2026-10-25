// Linux: D3DCompile and D3DCreateBlob for the D3D11 renderer (DXVK-native), on vkd3d-shader's
// plain C API (HLSL -> DXBC). vkd3d-utils has its own D3DCompile, but its COM objects use the
// Windows calling convention while DXVK-native's headers use the native one.
#include <d3dcompiler.h>
#include <vkd3d_shader.h>

#include "kernel/kernel.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace {

class Blob final : public ID3DBlob {
public:
    explicit Blob(size_t size) : data_(size) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
            AddRef();
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (!left) delete this;
        return left;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data_.size(); }

private:
    std::atomic<ULONG> references_{1};
    std::vector<uint8_t> data_;
};

Blob* makeBlob(const void* data, size_t size) {
    auto* blob = new Blob(size);
    if (size) std::memcpy(blob->GetBufferPointer(), data, size);
    return blob;
}

}  // namespace

extern "C" HRESULT WINAPI D3DCreateBlob(SIZE_T size, ID3DBlob** blob) {
    if (!blob) return E_POINTER;
    *blob = new Blob(size);
    return S_OK;
}

// Optimization flags have no vkd3d equivalent and are ignored; #include is not supported
// (the runtime's shaders are self-contained).
extern "C" HRESULT WINAPI D3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    (void)include;
    (void)flags1;
    (void)flags2;
    if (code) *code = nullptr;
    if (errors) *errors = nullptr;
    // Intrinsics vkd3d's HLSL compiler lacks (DXBC's ne is unordered: NaN != NaN).
    static const char kPrelude[] =
        "#define isnan(x) ((x) != (x))\n"
        "#define isfinite(x) (abs(x) < asfloat(0x7f800000u))\n"
        "#line 1\n";
    std::string text(kPrelude);
    text.append(static_cast<const char*>(data), size);
    data = text.data();
    size = text.size();
    std::vector<vkd3d_shader_macro> macros;
    for (const D3D_SHADER_MACRO* define = defines; define && define->Name; ++define)
        macros.push_back({define->Name, define->Definition ? define->Definition : ""});

    vkd3d_shader_hlsl_source_info hlsl{};
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.profile = target;
    hlsl.entry_point = entry;
    vkd3d_shader_preprocess_info preprocess{};
    preprocess.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    preprocess.next = &hlsl;
    preprocess.macros = macros.data();
    preprocess.macro_count = unsigned(macros.size());
    vkd3d_shader_compile_option options[] = {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_19},
        {VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1},
    };
    vkd3d_shader_compile_info info{};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &preprocess;
    info.source.code = data;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.options = options;
    info.option_count = sizeof(options) / sizeof(options[0]);
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = name ? name : "shader";

    vkd3d_shader_code output{};
    char* messages = nullptr;
    const int result = vkd3d_shader_compile(&info, &output, &messages);
    if (messages && errors && *messages) *errors = makeBlob(messages, std::strlen(messages) + 1);
    // Several callers fall back silently: every failure is logged here.
    if (result < 0)
        tz::logf("[d3dcompile] %s %s (%s) failed: %s", info.source_name, entry ? entry : "", target ? target : "",
                 messages ? messages : "");
    vkd3d_shader_free_messages(messages);
    if (result < 0) return E_FAIL;
    if (code) *code = makeBlob(output.code, output.size);
    vkd3d_shader_free_shader_code(&output);
    return S_OK;
}
