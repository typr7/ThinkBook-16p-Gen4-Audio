#pragma once
#include <stdint.h>
#include <stddef.h>

namespace gen4poll {
enum Result { Success, BadArgument, Timeout, Abort, FifoError, ShortTransfer };
// DesignWare registers. This engine never enables an interrupt or uses DMA.
enum : uint32_t { TAR=0x4, DATA=0x10, MASK=0x30, RAW=0x34, CLEAR=0x40,
 STOP_CLEAR=0x60, ENABLE=0x6c, STATUS=0x70, TXFLR=0x74, RXFLR=0x78,
 ABORT_SOURCE=0x80, ENABLE_STATUS=0x9c };

template<class IO> Result transfer(IO& io,uint8_t address,const uint8_t* write,size_t wn,
                                  uint8_t* read,size_t rn,unsigned txDepth,unsigned rxDepth) {
 if ((address!=0x40&&address!=0x41)||(!wn&&!rn)||(wn&&!write)||(rn&&!read)||
     wn>4096||rn>4096||!txDepth||!rxDepth||txDepth>256||rxDepth>256) return BadArgument;
 io.write(MASK,0);
 io.write(ENABLE,0);
 unsigned steps=0;
 auto wait=[&]() { return ++steps<=20000 && io.wait(); };
 while(io.read(ENABLE_STATUS)&1) if(!wait())return Timeout;
 io.read(CLEAR);
 io.write(TAR,address);
 io.write(ENABLE,1);
 Result result=Timeout;
 size_t sent=0,issued=0,received=0;
 bool stop=false;
 while(wait()) {
  const uint32_t raw=io.read(RAW);
  if(raw==0xFFFFFFFFU) { result=FifoError;break; }
  if(raw&(1U<<6)) { io.read(ABORT_SOURCE);result=Abort;break; }
  if(raw&((1U<<1)|(1U<<3))) { result=FifoError;break; }
  unsigned available=io.read(RXFLR);
  if(available>rxDepth||available>issued-received) { result=FifoError;break; }
  while(available--)read[received++]=uint8_t(io.read(DATA));
  const unsigned occupied=io.read(TXFLR);
  if(occupied>txDepth) { result=FifoError;break; }
  unsigned room=txDepth-occupied;
  while(room--) {
   if(sent<wn) {
    uint32_t command=write[sent++];
    if(sent==wn&&!rn)command|=1U<<9;
    io.write(DATA,command);
   } else if(issued<rn&&issued-received<rxDepth) {
    uint32_t command=1U<<8;
    if(issued==0&&wn)command|=1U<<10;
    if(++issued==rn)command|=1U<<9;
    io.write(DATA,command);
   } else break;
  }
  if(raw&(1U<<9))stop=true;
  if(stop) {
   if(sent==wn&&issued==rn&&received==rn&&!(io.read(STATUS)&(1U<<5)))result=Success;
   else result=ShortTransfer;
   break;
  }
 }
 io.write(MASK,0);
 io.write(ENABLE,0);
 // Cleanup is also bounded; acknowledge status only after the adapter is off.
 unsigned cleanup=0;
 while(io.read(ENABLE_STATUS)&1) {
  if(++cleanup>2000||!io.wait()) { result=Timeout;break; }
 }
 io.read(CLEAR);
 return result;
}
}
