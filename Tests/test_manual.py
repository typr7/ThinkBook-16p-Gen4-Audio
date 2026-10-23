from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]/'Audio'
s=(root/'CirrusAudioFixup.cpp').read_text()
def function(signature):
 start=s.index(signature);brace=s.index('{',start);end=brace+1;level=1
 while level:
  if s[end]=='{':level+=1
  if s[end]=='}':level-=1
  end+=1
 return s[start:end]
start=function('bool CirrusAudioFixup::start(')
assert all(x not in start for x in ['detectAmplifiers()','fullDriverFlow()','validateOwnCalibration()','setTimeoutMS('])
assert 'mWorkLoop = IOWorkLoop::workLoop();' in function('bool CirrusAudioFixup::setupProbeTimer(')
assert 'getWorkLoop()' not in function('bool CirrusAudioFixup::setupProbeTimer(')
assert '!mOwnCalibrationValidated || !mManualArmed' in function('bool CirrusAudioFixup::transferToAddress(')
assert 'kIOClientPrivilegeAdministrator' in function('IOReturn CirrusAudioFixup::setProperties(')
mock=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
using IOReturn=int;
enum {kIOReturnSuccess,kIOReturnOffline,kIOReturnIOError,kIOReturnBadArgument,kIOReturnNotReady,kIOReturnBusy};
struct Amp {bool initialized=false,firmwareValidated=false,dspAlive=false,playbackFaulted=false;unsigned cleanupAttempts=0;};
struct Timer {unsigned schedules=0,cancels=0;void setTimeoutMS(unsigned){++schedules;}void cancelTimeout(){++cancels;}};
class CirrusAudioFixup {
public:
 bool mStopping=false,mPowerAvailable=true,mManualArmed=false,mOwnCalibrationValidated=false;
 bool calibration=true,initialization=true,stopOK=true;
 unsigned reads=0,writes=0;size_t mAmpCount=2;Amp mAmps[2];Timer timer;Timer* mProbeTimer=&timer;
 std::map<std::string,std::string> properties;
 void setProperty(const char* n,const char* v){properties[n]=v;}
 void detectAmplifiers(){++reads;}
 bool bootArgEnabled(const char*){return false;}
 bool validateOwnCalibration(){++reads;mOwnCalibrationValidated=calibration;return calibration;}
 void fullDriverFlow(){assert(mManualArmed&&mOwnCalibrationValidated);++writes;for(auto& a:mAmps)a.initialized=a.firmwareValidated=a.dspAlive=initialization;}
 bool stopPlayback(Amp&){assert(mManualArmed&&mOwnCalibrationValidated);++writes;return stopOK;}
 bool stopDSP(Amp&){assert(mManualArmed&&mOwnCalibrationValidated);++writes;return stopOK;}
 IOReturn runManualAction(uintptr_t);
};
'''
tests=r'''
int main(){
 CirrusAudioFixup d;assert(d.runManualAction(4)==kIOReturnSuccess&&d.writes==0&&d.reads==0);
 assert(d.runManualAction(1)==kIOReturnSuccess&&d.writes==0&&!d.mManualArmed&&!d.mOwnCalibrationValidated);
 assert(d.runManualAction(2)==kIOReturnSuccess&&d.mManualArmed&&d.mOwnCalibrationValidated&&d.timer.schedules==0);
 assert(d.runManualAction(2)==kIOReturnBusy);assert(d.runManualAction(3)==kIOReturnSuccess&&d.timer.schedules==1);
 assert(d.runManualAction(4)==kIOReturnSuccess&&!d.mManualArmed&&!d.mOwnCalibrationValidated);
 CirrusAudioFixup bad;bad.calibration=false;assert(bad.runManualAction(3)==kIOReturnNotReady&&bad.writes==0&&!bad.mManualArmed&&bad.timer.schedules==0);
 CirrusAudioFixup fail;fail.initialization=false;assert(fail.runManualAction(3)==kIOReturnIOError&&fail.timer.schedules==0);
 assert(fail.runManualAction(3)==kIOReturnNotReady&&fail.timer.schedules==0);
 fail.stopOK=false;assert(fail.runManualAction(4)==kIOReturnIOError&&!fail.mManualArmed);
 CirrusAudioFixup off;off.mPowerAvailable=false;assert(off.runManualAction(3)==kIOReturnOffline&&off.reads==0&&off.writes==0);
 CirrusAudioFixup invalid;assert(invalid.runManualAction(99)==kIOReturnBadArgument&&invalid.writes==0&&invalid.reads==0);
 puts("PASS actual manual-control method: no startup transactions/timer; private work loop; admin-only actions; read-only probe; explicit initialization; arm only after both DSPs ready; calibration/init failures do not start monitor; stop/wake requires rearm.");
}
'''
with tempfile.TemporaryDirectory(prefix='gen4-manual-') as d:
 p=Path(d);(p/'test.cpp').write_text(mock+function('IOReturn CirrusAudioFixup::runManualAction(')+tests)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
