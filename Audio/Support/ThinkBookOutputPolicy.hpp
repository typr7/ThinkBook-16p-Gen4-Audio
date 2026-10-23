#pragma once
#include <stdint.h>
#include <stddef.h>

// DSP playback trial: upstream normal volume/gain; mute and idle gain remain allowed.
// ASP TX remains disabled pending board slot verification.
// Applies at the final I2C boundary, also to firmware-originated bulk writes.
inline bool thinkbookOutputWriteAllowed(uint32_t reg, const uint8_t* data, size_t length) {
    if (!data || length == 0 || length > 0xFFFFU - 4U || uint64_t(reg) + length > 0x100000000ULL)
        return false;
    const uint32_t guarded[] = {0x6000, 0x6c04, 0x4800};
    for (uint32_t target : guarded) {
        if (uint64_t(reg) < uint64_t(target) + 4 && uint64_t(target) < uint64_t(reg) + length) {
            if (reg != target || length != 4) return false;
            uint32_t value = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                             (uint32_t(data[2]) << 8) | data[3];
            if (target == 0x6000 && value != 0xA678 && value != 0x8000) return false;
            if (target == 0x6c04 && value != 0 && value != 0x233) return false;
            if (target == 0x4800 && value != 0 && value != 0x10000) return false;
        }
    }
    return true;
}
