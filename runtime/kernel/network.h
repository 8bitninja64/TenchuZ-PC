#pragma once
#include <cstdint>

namespace tz::net {
// Opt-in experimental System Link. Explicit local address avoids choosing
// the wrong interface on PCs with VPNs or when testing two local instances.
bool enabled();
// Send TCP FIN before joining a worker that drains incoming data to EOF.
void beginDisconnect(uint32_t socketHandle);
uint64_t playerId();
const char* playerName();
void titleAddress(uint32_t address);
void randomBytes(uint32_t address, uint32_t count);
uint32_t dispatch(uint32_t app, uint32_t message, uint32_t buffer, uint32_t size);
}
