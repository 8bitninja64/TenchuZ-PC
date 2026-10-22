// Disassembles, translates and test-compiles shader microcode dumped with
// TENCHU_DUMP_SHADERS. File names starting with ps_ are pixel shaders.
//   XenosDisasm <file.bin>...              disassembly
//   XenosDisasm --hlsl <file.bin>...       translated HLSL
//   XenosDisasm --compile <file.bin>...    compile with D3DCompile, report failures
#include "gpu/xenos/translator.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
    enum class Mode { Disassemble, Hlsl, Compile } mode = Mode::Disassemble;
    int failures = 0, total = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--hlsl")) { mode = Mode::Hlsl; continue; }
        if (!strcmp(argv[i], "--compile")) { mode = Mode::Compile; continue; }
        std::ifstream file(argv[i], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> words(bytes.size() / 4);
        for (size_t w = 0; w < words.size(); ++w)
            words[w] = uint32_t(bytes[w * 4]) << 24 | uint32_t(bytes[w * 4 + 1]) << 16 |
                       uint32_t(bytes[w * 4 + 2]) << 8 | bytes[w * 4 + 3];
        const bool pixel = std::filesystem::path(argv[i]).filename().string().rfind("ps_", 0) == 0;
        const auto program = tz::xenos::decode(pixel ? tz::xenos::ShaderType::Pixel : tz::xenos::ShaderType::Vertex,
                                               words);
        if (mode == Mode::Disassemble) {
            printf("; %s (%zu dwords)\n%s\n", argv[i], words.size(), tz::xenos::disassemble(program).c_str());
            continue;
        }
        const auto translated = tz::xenos::translate(program);
        if (mode == Mode::Hlsl) {
            printf("// %s\n%s\n", argv[i], translated.hlsl.c_str());
            continue;
        }
        ++total;
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(translated.hlsl.data(), translated.hlsl.size(), argv[i], nullptr, nullptr,
                                      "main", pixel ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                                      &errors);
        if (FAILED(hr) || !translated.error.empty()) {
            ++failures;
            printf("FAIL %s%s%s\n", argv[i], translated.error.empty() ? "" : (" (" + translated.error + ")").c_str(),
                   errors ? "" : "");
            if (errors) printf("%.*s\n", int(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
        }
        if (code) code->Release();
        if (errors) errors->Release();
    }
    if (mode == Mode::Compile) printf("%d of %d shaders compiled\n", total - failures, total);
    return failures ? 1 : 0;
}
