#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include "../Audio/Support/OwnCalibration.hpp"
#include "../Audio/Devices/CS35L41/Hardware/OTPMap.hpp"
static void put(std::array<uint8_t,48>& b,unsigned offset,uint32_t x){for(unsigned i=0;i<4;++i)b[offset+i]=uint8_t(x>>(8*i));}
int main(){
 uint64_t otpUid=0;
 for (uint32_t mapId:{1,2,3,6,8}) {
  assert(ownUidFromOtp(mapId,0x667788AB,0x11223344,otpUid));
  assert(otpUid==0x11223344667788ULL);
  assert(ownUidFromOtp(mapId,0x66778800,0x11223344,otpUid)&&otpUid==0x11223344667788ULL);
 }
 assert(!ownUidFromOtp(9,0x667788AB,0x11223344,otpUid));
 assert(!ownUidFromOtp(1,0,0,otpUid));
 assert(!ownUidFromOtp(1,0xFFFFFFFF,0xFFFFFFFF,otpUid));
 for (const auto& map:kCs35l41OtpMapEntries) {
  for (unsigned bit=0;bit<64;++bit) {
   const uint64_t fuse=uint64_t(1)<<bit;
   std::array<uint32_t,80> memory{};
   memory[22]=uint32_t(fuse);memory[23]=uint32_t(fuse>>32);
   unsigned sourceBit=unsigned(map.word_offset)*32+map.bit_offset;
   uint32_t low=0,high=0;
   for (unsigned e=0;e<map.num_elements;++e) {
    const auto& element=map.map[e];uint32_t value=0;
    for (unsigned j=0;j<element.size;++j) {
     const unsigned p=sourceBit+j;
     value|=((memory[p/32]>>(p%32))&1U)<<j;
    }
    sourceBit+=element.size;
    if(element.reg==0x17040)low|=value<<element.shift;
    if(element.reg==0x17044)high|=value<<element.shift;
   }
   const uint64_t expected=(uint64_t(high)<<32)|low;
   const bool decoded=ownUidFromOtp(map.id,memory[22],memory[23],otpUid);
   assert(decoded==(expected!=0));if(decoded)assert(otpUid==expected);
  }
 }
 puts("PASS raw OTP UID: all five supported map IDs, cross-word extraction, non-UID byte ignored, unknown map and blank fuse rejected.");
 std::array<uint8_t,48> valid{};put(valid,0,48);put(valid,4,2);put(valid,8,1);put(valid,16,7);valid[24]=23;valid[25]=1;valid[26]=21;put(valid,28,2);put(valid,36,8);valid[44]=23;valid[45]=1;valid[46]=22;
 OwnAmpCalibration out{};assert(matchOwnAmpCalibration(valid.data(),48,1,out)&&out.resistance==21);assert(matchOwnAmpCalibration(valid.data(),48,2,out)&&out.resistance==22);
 assert(!matchOwnAmpCalibration(valid.data(),48,3,out));assert(!matchOwnAmpCalibration(valid.data(),47,1,out));assert(!matchOwnAmpCalibration(nullptr,48,1,out));
 auto x=valid;put(x,28,1);assert(!matchOwnAmpCalibration(x.data(),48,1,out));x=valid;x[25]=0;assert(!matchOwnAmpCalibration(x.data(),48,1,out));x=valid;x[26]=0;assert(!matchOwnAmpCalibration(x.data(),48,1,out));x=valid;put(x,16,0);assert(!matchOwnAmpCalibration(x.data(),48,1,out));x=valid;put(x,0,49);assert(!matchOwnAmpCalibration(x.data(),48,1,out));
 puts("PASS own-calibration: exact UID selection; unknown UID; truncated or invalid buffer; duplicate target; failed status; zero resistance; missing timestamp; header mismatch.");
}
