// printf-style formatting with guest arguments.
#include "kernel.h"
#include <cstdio>
#include <cstring>

namespace tz {

GuestArgs GuestArgs::fromRegisters(const PPCContext& ctx, int firstRegister) {
    GuestArgs args;
    args.ctx = &ctx;
    args.index = firstRegister - 3;
    return args;
}

GuestArgs GuestArgs::fromVaList(uint32_t vaList) {
    GuestArgs args;
    args.vaList = vaList;
    return args;
}

uint64_t GuestArgs::next() {
    if (!ctx) {
        // va_list: consecutive 8-byte big-endian slots.
        const uint64_t value = load64(vaList);
        vaList += 8;
        return value;
    }
    const int slot = index++;
    const PPCRegister* registers[] = {&ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6,
                                      &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10};
    if (slot < 8) return registers[slot]->u64;
    // Arguments past r10 live in the caller's parameter area.
    return load64(ctx->r1.u32 + 0x50 + uint32_t(slot - 8) * 8);
}

namespace {
std::string utf16ToNarrow(const std::u16string& text) {
    std::string result;
    result.reserve(text.size());
    for (char16_t c : text) result.push_back(c < 0x80 ? char(c) : '?');
    return result;
}
}  // namespace

std::string formatGuest(const std::string& format, GuestArgs& args) {
    std::string out;
    char buffer[512];
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out.push_back(format[i]);
            continue;
        }
        if (i + 1 < format.size() && format[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }
        // %[flags][width][.precision][length]conversion
        std::string spec = "%";
        size_t j = i + 1;
        while (j < format.size() && std::strchr("-+ #0", format[j])) spec.push_back(format[j++]);
        auto number = [&] {
            if (j < format.size() && format[j] == '*') {
                spec += std::to_string(int32_t(args.next()));
                ++j;
                return;
            }
            while (j < format.size() && isdigit(uint8_t(format[j]))) spec.push_back(format[j++]);
        };
        number();
        if (j < format.size() && format[j] == '.') {
            spec.push_back(format[j++]);
            number();
        }
        enum { Default, Short, Long, LongLong, Wide } length = Default;
        while (j < format.size()) {
            if (format[j] == 'h') { length = length == Default ? Short : length; ++j; }
            else if (format[j] == 'l') { length = length == Long ? LongLong : Long; ++j; }
            else if (format[j] == 'L' || format[j] == 'q' || format[j] == 'j') { length = LongLong; ++j; }
            else if (format[j] == 'w') { length = Wide; ++j; }
            else if (format[j] == 'z' || format[j] == 't') { ++j; }
            else if (format.compare(j, 3, "I64") == 0) { length = LongLong; j += 3; }
            else if (format.compare(j, 3, "I32") == 0) { j += 3; }
            else break;
        }
        if (j >= format.size()) break;
        const char conversion = format[j];
        i = j;
        switch (conversion) {
        case 'd':
        case 'i': {
            const uint64_t raw = args.next();
            const long long value = length == LongLong ? (long long)raw
                                  : length == Short ? (long long)int16_t(raw) : (long long)int32_t(raw);
            snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), value);
            out += buffer;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            const uint64_t raw = args.next();
            const unsigned long long value = length == LongLong ? raw
                                           : length == Short ? uint16_t(raw) : uint32_t(raw);
            snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(), value);
            out += buffer;
            break;
        }
        case 'p':
            snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.next()));
            out += buffer;
            break;
        case 'c':
        case 'C': {
            const char value = char(args.next());
            snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), value);
            out += buffer;
            break;
        }
        case 's':
        case 'S': {
            const uint32_t pointer = uint32_t(args.next());
            const bool wide = (conversion == 'S') != (length == Long || length == Wide);
            std::string text = !pointer ? "(null)" : wide ? utf16ToNarrow(loadWideString(pointer)) : loadString(pointer);
            std::string formatted(text.size() + 512, '\0');
            const int written = snprintf(formatted.data(), formatted.size(), (spec + "s").c_str(), text.c_str());
            formatted.resize((std::max)(written, 0));
            out += formatted;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            const uint64_t raw = args.next();
            double value;
            std::memcpy(&value, &raw, sizeof(value));
            snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), value);
            out += buffer;
            break;
        }
        case 'n':
            args.next();
            break;
        default:
            out += spec;
            out.push_back(conversion);
            break;
        }
    }
    return out;
}

}  // namespace tz
