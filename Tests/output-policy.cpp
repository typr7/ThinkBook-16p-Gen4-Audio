#include "../Audio/Support/ThinkBookOutputPolicy.hpp"
#include <assert.h>
#include <stdio.h>
static bool allowed(uint32_t reg, uint32_t value) {
    uint8_t bytes[] = {uint8_t(value>>24),uint8_t(value>>16),uint8_t(value>>8),uint8_t(value)};
    return thinkbookOutputWriteAllowed(reg, bytes, sizeof(bytes));
}
int main() {
    for (uint32_t value=0; value<65536; ++value) {
        assert(allowed(0x6000,value)==(value==0xA678||value==0x8000));
        assert(allowed(0x6c04,value)==(value==0||value==0x233));
        assert(allowed(0x4800,value)==(value==0));
    }
    assert(allowed(0x4800,0x10000));
    assert(!allowed(0x4800,0x10001));
    assert(!allowed(0x6000,0x0100BA00));
    assert(allowed(0x6c04,0x233));
    assert(!allowed(0x6c04,0x234));
    uint8_t bulk[60]={};
    for(uint32_t target : {0x6000U,0x6c04U,0x4800U}) {
        for (int offset=-59;offset<4;++offset)
            assert(!thinkbookOutputWriteAllowed(target+offset,bulk,sizeof(bulk)));
    }
    assert(thinkbookOutputWriteAllowed(0x3800000,bulk,sizeof(bulk)));
    assert(!thinkbookOutputWriteAllowed(0xfffffff0,bulk,sizeof(bulk)));
    assert(!thinkbookOutputWriteAllowed(0x6000,nullptr,4));
    puts("PASS: exhaustive 16-bit volume/gain/TX values, overlapping bulk writes, overflow, null input");
}
