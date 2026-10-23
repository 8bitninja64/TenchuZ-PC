#pragma once
// XMA decoder hardware registers (0x7FEA0000-0x7FEAFFFF).
#include <cstdint>

namespace tz::xma {

bool ownsAddress(uint32_t address);
uint32_t readRegister(uint32_t address);
void writeRegister(uint32_t address, uint32_t value);

}  // namespace tz::xma
