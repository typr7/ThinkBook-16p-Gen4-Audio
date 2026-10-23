"""Compile the actual stop/mailbox code with mocked register IO; no native bus test."""
from pathlib import Path
import subprocess,tempfile
p=Path(__file__).with_name('check_runtime.py');text=p.read_text();scope={'__file__':str(p)}
exec(compile(text.split("with tempfile.TemporaryDirectory(prefix='cirrus-runtime-check-')")[0],str(p),'exec'),scope)
functions='\n'.join(scope['function'](n) for n in ['stopPlayback','sendMailboxCommand','unlockTestKey','lockTestKey','stopDSP','publishDriverVerdict'])
checks=r'''
int main(){
 for(bool mailbox:{true,false}){
  CirrusAudioFixup d;auto &a=d.mAmps[0];a.firmwareIdVersion=0x2b01;a.dspAlive=true;a.firmwareValidated=true;a.playbackActive=true;
  d.regs[0x13004]=0;d.regs[0x2014]=1;d.regs[0x2018]=0x3001;d.regs[0x6000]=0xBA00;d.regs[0x6c04]=0;d.mailbox=mailbox;
  bool ok=d.stopPlayback(a);if(ok!=mailbox){printf("mailbox=%d ok=%d pwr1=%x pwr2=%x vol=%x gain=%x status=%x writes=%zu\n",mailbox,ok,d.regs[0x2014],d.regs[0x2018],d.regs[0x6000],d.regs[0x6c04],d.regs[0x13004],d.writes.size());for(auto w:d.writes)printf("%x=%x\n",w.first,w.second);fflush(stdout);}assert(ok==mailbox);
  unsigned pause=0;
  for(auto w:d.writes){if(w.first==0x13020){assert(w.second!=8);assert(w.second==1);++pause;} if(w.first==0x6000)assert(w.second==0xA678);if(w.first==0x6c04)assert(w.second==0);}
  assert(pause==1);assert(!(d.regs[0x2014]&1));assert(!(d.regs[0x2018]&0x3031));
  if(mailbox)assert(!a.playbackActive);else assert(a.playbackFaulted);
 }
 puts("PASS: production stop code sends PAUSE only, no undefined command 8; muted shutdown and failed-ack latch verified");
}
'''
source=scope['preamble']+scope['LEGACY_CONSTANTS']+scope['mock']+'\n'+scope['declarations']+'\n};\n'+scope['adapter']+functions+checks
with tempfile.TemporaryDirectory(prefix='cirrus-stop-backport-') as t:
 f=Path(t)/'test.cpp';f.write_text(source);exe=Path(t)/'test'
 subprocess.run(['clang++','-std=c++17','-O0','-I'+str(scope['ROOT']/'Audio'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
