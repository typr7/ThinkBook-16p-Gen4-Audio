"""Host-test the actual preflight functions with read-only mocked registers/NVRAM."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]/'Audio'
source=(root/'CirrusAudioFixup.cpp').read_text()
def function(name):
    start=source.index('bool CirrusAudioFixup::'+name+'(')
    brace=source.index('{',start);level=1;end=brace+1
    while level:
        if source[end]=='{':level+=1
        if source[end]=='}':level-=1
        end+=1
    return source[start:end]
mock=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "Support/OwnCalibration.hpp"
struct OSObject { virtual ~OSObject(){}; void release(){}; };
struct OSData:OSObject {
 std::array<uint8_t,48> raw{};unsigned length=48;
 unsigned getLength(){return length;}
 const void* getBytesNoCopy(){return raw.data();}
};
struct IORegistryEntry:OSObject {
 static IORegistryEntry options;static bool available,present;
 static OSData data;
 static IORegistryEntry* fromPath(const char*){return available?&options:nullptr;}
 OSObject* copyProperty(const char*){return present?&data:nullptr;}
};
IORegistryEntry IORegistryEntry::options;
OSData IORegistryEntry::data;
bool IORegistryEntry::available=true,IORegistryEntry::present=true;
#define OSDynamicCast(T,x) dynamic_cast<T*>(x)
struct AmplifierState { const char* name;uint8_t address; };
class CirrusAudioFixup {
public:
 bool mOwnCalibrationValidated=false;
 size_t mAmpCount=2;
 AmplifierState mAmps[2]={{"left",0x40},{"right",0x41}};
 std::map<unsigned,std::map<uint32_t,uint32_t>> regs;
 std::vector<uint32_t> reads;
 std::map<std::string,std::string> properties;
 bool readRegister(AmplifierState& a,uint32_t reg,uint32_t* value){
  reads.push_back(reg);auto it=regs[a.address].find(reg);
  if(it==regs[a.address].end())return false;*value=it->second;return true;
 }
 void setProperty(const char* name,const char* value){properties[name]=value;}
 void setProperty(const char* name,bool value){properties[name]=value?"true":"false";}
 bool readOwnCalibration(AmplifierState&,OwnAmpCalibration&);
 bool validateOwnCalibration();
};
// No writeRegister/bulkWrite method exists in this mock: preflight must be read-only.
static void put(unsigned p,uint32_t x){auto& b=IORegistryEntry::data.raw;for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(x>>(i*8));}
static CirrusAudioFixup fixture(){
 IORegistryEntry::available=IORegistryEntry::present=true;IORegistryEntry::data=OSData{};
 put(0,48);put(4,2);
 // Deliberately reversed EFI record order; match by UID, never speaker index.
 put(8,2);put(16,1);IORegistryEntry::data.raw[24]=23;IORegistryEntry::data.raw[25]=1;IORegistryEntry::data.raw[26]=22;
 put(28,1);put(36,1);IORegistryEntry::data.raw[44]=23;IORegistryEntry::data.raw[45]=1;IORegistryEntry::data.raw[46]=21;
 CirrusAudioFixup d;
 for(unsigned address:{0x40U,0x41U}){
  auto& r=d.regs[address];r[0]=0x35A40;r[4]=0xB2;r[0x10]=1;
  r[0x458]=(address==0x40?1U:2U)<<8;r[0x45C]=0;
  // DIE_STS is intentionally blank before OTP unpacking.
  r[0x17040]=r[0x17044]=0;
 }
 return d;
}
'''
tests=r'''
int main(){
 OwnAmpCalibration out{};
 auto d=fixture();assert(d.validateOwnCalibration());assert(d.mOwnCalibrationValidated);
 assert(d.properties["ThinkBook_Calibration_Detail_left"]=="RAW_OTP_UID_MATCHED_FACTORY_RECORD");
 assert(!d.readOwnCalibration(d.mAmps[0],out)); // Must recheck DIE_STS after init.
 assert(d.properties["ThinkBook_Calibration_Detail_left"]=="DIE_UID_NO_VALID_FACTORY_MATCH_AFTER_INIT");
 d.regs[0x40][0x17040]=1;d.regs[0x41][0x17040]=2;
 assert(d.readOwnCalibration(d.mAmps[0],out)&&out.resistance==21);
 assert(d.readOwnCalibration(d.mAmps[1],out)&&out.resistance==22);
 auto bad=fixture();bad.regs[0x41][0x458]=3<<8;
 assert(!bad.validateOwnCalibration()&&!bad.mOwnCalibrationValidated);
 bad=fixture();bad.regs[0x41][0x458]=1<<8;
 assert(!bad.validateOwnCalibration()&&!bad.mOwnCalibrationValidated);
 bad=fixture();bad.regs[0x40].erase(0x458);assert(!bad.validateOwnCalibration());
 assert(bad.properties["ThinkBook_Calibration_Detail_left"]=="RAW_OTP_READ_FAILED");
 bad=fixture();bad.regs[0x40][0x10]=9;assert(!bad.validateOwnCalibration());
 bad=fixture();IORegistryEntry::present=false;assert(!bad.validateOwnCalibration());
 assert(bad.properties["ThinkBook_Calibration_Detail_left"]=="NVRAM_CALIBRATION_MISSING");
 bad=fixture();IORegistryEntry::available=false;assert(!bad.validateOwnCalibration());
 bad=fixture();IORegistryEntry::data.length=47;assert(!bad.validateOwnCalibration());
 bad=fixture();bad.mAmpCount=1;assert(!bad.validateOwnCalibration());
 puts("PASS actual preflight functions: read-only OTP before initialization; reversed EFI record order; DIE UID recheck after init; missing reads/variables, malformed data, unknown/duplicate UID or OTP map fail closed.");
}
'''
with tempfile.TemporaryDirectory(prefix='gen4-preflight-') as t:
    p=Path(t);(p/'test.cpp').write_text(mock+function('readOwnCalibration')+'\n'+function('validateOwnCalibration')+tests)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root.resolve()),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
