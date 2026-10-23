#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int stopping=0;
static void output(void* context,AudioQueueRef queue,AudioQueueBufferRef buffer){
 (void)context;if(stopping)return;
 memset(buffer->mAudioData,0,buffer->mAudioDataBytesCapacity);
 buffer->mAudioDataByteSize=buffer->mAudioDataBytesCapacity;
 AudioQueueEnqueueBuffer(queue,buffer,0,NULL);
}
int main(int argc,char**argv){
 double seconds=argc>1?atof(argv[1]):180;
 if(seconds<=0||seconds>600)return 2;
 AudioStreamBasicDescription format={0};
 format.mSampleRate=48000;format.mFormatID=kAudioFormatLinearPCM;
 format.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked;
 format.mFramesPerPacket=1;format.mChannelsPerFrame=2;format.mBitsPerChannel=16;
 format.mBytesPerFrame=format.mBytesPerPacket=4;
 AudioQueueRef queue=NULL;
 OSStatus status=AudioQueueNewOutput(&format,output,NULL,NULL,NULL,0,&queue);
 if(status){fprintf(stderr,"AudioQueueNewOutput: %d\n",status);return 1;}
 CFStringRef device=CFSTR("AppleHDAEngineOutput:1F,3,0,1,1:0");
 status=AudioQueueSetProperty(queue,kAudioQueueProperty_CurrentDevice,&device,sizeof(device));
 if(status){fprintf(stderr,"Select built-in output: %d\n",status);AudioQueueDispose(queue,true);return 1;}
 for(unsigned i=0;i<3;++i){
  AudioQueueBufferRef buffer;status=AudioQueueAllocateBuffer(queue,4096,&buffer);
  if(status){AudioQueueDispose(queue,true);return 1;}output(NULL,queue,buffer);
 }
 status=AudioQueueStart(queue,NULL);
 if(status){fprintf(stderr,"AudioQueueStart: %d\n",status);AudioQueueDispose(queue,true);return 1;}
 puts("AUDIO_CLOCK_READY");fflush(stdout);
 CFRunLoopRunInMode(kCFRunLoopDefaultMode,seconds,false);
 stopping=1;AudioQueueStop(queue,true);AudioQueueDispose(queue,true);return 0;
}
