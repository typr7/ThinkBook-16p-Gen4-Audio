#pragma once
#include <stdint.h>
#include <stddef.h>
struct OwnAmpCalibration { uint64_t uid; int8_t ambient; uint8_t status; uint16_t resistance; };
// Both Linux CS35L41 OTP maps place the 56 UID bits at OTP[22] bit 8.
// Reading the fuse words avoids depending on DIE_STS registers before unpackOTP.
inline bool ownUidFromOtp(uint32_t mapId,uint32_t word22,uint32_t word23,uint64_t& uid) {
    if (mapId!=1&&mapId!=2&&mapId!=3&&mapId!=6&&mapId!=8) return false;
    const uint64_t value=(uint64_t(word23)<<24)|(word22>>8);
    if (!value||value==0x00FFFFFFFFFFFFFFULL) return false;
    uid=value;
    return true;
}
inline uint32_t ownLE32(const uint8_t* b) { return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24; }
inline bool matchOwnAmpCalibration(const uint8_t* raw,size_t length,uint64_t uid,OwnAmpCalibration& out) {
    if (!raw||length!=48||ownLE32(raw)!=length||ownLE32(raw+4)!=2||!uid) return false;
    unsigned matches=0;uint64_t targets[2]{};
    for (unsigned i=0;i<2;++i) {
        const uint8_t* p=raw+8+i*20;
        const uint64_t target=uint64_t(ownLE32(p))|uint64_t(ownLE32(p+4))<<32;
        targets[i]=target;
        const uint16_t resistance=static_cast<uint16_t>(uint32_t(p[18])|(uint32_t(p[19])<<8));
        if (!target||(!ownLE32(p+8)&&!ownLE32(p+12))||p[17]!=1||!resistance||resistance==0xFFFF) return false;
        if (target==uid) { ++matches;out={target,static_cast<int8_t>(p[16]),p[17],resistance}; }
    }
    return matches==1&&targets[0]!=targets[1];
}
