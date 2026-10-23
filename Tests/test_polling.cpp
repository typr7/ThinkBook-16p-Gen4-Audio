#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>
#include <utility>
#include "../Vendor/VoodooI2C/VoodooI2C/VoodooI2C/VoodooI2CController/ThinkBookPolledTransfer.hpp"
struct Model {
 unsigned step=0,limit=30000,enable=0;bool stuckEnable=false,stuckTx=false,nack=false,earlyStop=false;
 unsigned depth=4;bool stopped=false;uint32_t errors=0;unsigned generated=0;
 std::deque<uint32_t> tx;std::deque<uint8_t> rx;
 std::vector<uint32_t> commands;std::vector<std::pair<uint32_t,uint32_t>> writes;
 uint32_t read(uint32_t reg) {
  using namespace gen4poll;
  if(reg==ENABLE_STATUS)return stuckEnable?1:enable;
  if(reg==RAW)return errors|(stopped?(1<<9):0)|(earlyStop?(1<<9):0);
  if(reg==TXFLR)return stuckTx?depth:unsigned(tx.size());
  if(reg==RXFLR)return unsigned(rx.size());
  if(reg==DATA){assert(!rx.empty());auto v=rx.front();rx.pop_front();return v;}
  if(reg==STATUS)return tx.empty()?0:1<<5;
  if(reg==CLEAR){stopped=false;errors=0;return 0;}
  return 0;
 }
 void write(uint32_t reg,uint32_t value) {
  using namespace gen4poll;writes.emplace_back(reg,value);
  if(reg==MASK)assert(value==0);
  if(reg==ENABLE){enable=value;if(!enable){tx.clear();rx.clear();}}
  if(reg==DATA){assert(tx.size()<depth);tx.push_back(value);commands.push_back(value);}
 }
 bool wait() {
  if(++step>limit)return false;
  if(nack&&step==3)errors|=1<<6;
  // Delayed consumption models TX FIFO backpressure and a slower target.
  if(enable&&!stuckTx&&step%3==0&&!tx.empty()) {
   auto command=tx.front();tx.pop_front();
   if(command&(1<<8)){assert(rx.size()<depth);rx.push_back(uint8_t(0x80+generated++));}
   if(command&(1<<9))stopped=true;
  }
  return true;
 }
};
int main(){
 using namespace gen4poll;
 for(unsigned depth:{1U,4U,16U})for(unsigned wn:{0U,4U,132U})for(unsigned rn:{0U,4U,320U}) {
  if(!wn&&!rn)continue;
  Model m;m.depth=depth;
  std::vector<uint8_t>w(wn),r(rn);for(unsigned i=0;i<wn;++i)w[i]=uint8_t(i);
  assert(transfer(m,0x40,w.data(),wn,r.data(),rn,depth,depth)==Success);
  assert(m.commands.size()==wn+rn);unsigned stops=0,restarts=0;
  for(unsigned i=0;i<m.commands.size();++i){
   const auto c=m.commands[i];if(c&(1<<9)){++stops;assert(i+1==m.commands.size());}
   if(c&(1<<10)){++restarts;assert(wn&&rn&&i==wn);}
   if(i<wn)assert((c&255)==w[i]);else assert(c&(1<<8));
  }
  assert(stops==1&&restarts==(wn&&rn?1U:0U));
  for(unsigned i=0;i<rn;++i)assert(r[i]==uint8_t(0x80+i));
  assert(m.enable==0);
 }
 uint8_t w[4]={0,0,0,0},r[4]{};
 Model m;assert(transfer(m,0x42,w,4,r,4,4,4)==BadArgument&&m.writes.empty());
 assert(transfer(m,0x40,nullptr,4,r,4,4,4)==BadArgument&&m.writes.empty());
 assert(transfer(m,0x40,w,4097,r,4,4,4)==BadArgument&&m.writes.empty());
 m=Model{};m.nack=true;assert(transfer(m,0x40,w,4,r,4,4,4)==Abort);assert(!m.enable);
 m=Model{};m.stuckTx=true;m.limit=30;assert(transfer(m,0x40,w,4,r,4,4,4)==Timeout);assert(!m.enable);
 m=Model{};m.stuckEnable=true;m.limit=30;assert(transfer(m,0x40,w,4,r,4,4,4)==Timeout);
 m=Model{};m.earlyStop=true;assert(transfer(m,0x40,w,4,r,4,4,4)==ShortTransfer);assert(!m.enable);
 puts("PASS polled transport: FIFO depths 1/4/16, combined reads, 132-byte writes, 320-byte reads, FIFO backpressure, repeated START, final STOP, NACK/stall/short transfer and invalid requests; no interrupt mask enabled.");
}
