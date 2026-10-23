#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

static int supported_driver(io_service_t service) {
    CFTypeRef version=IORegistryEntryCreateCFProperty(service,CFSTR("ThinkBookAudioVersion"),NULL,0);
    if(version) {
        int valid=CFEqual(version,CFSTR("1.0.0"));CFRelease(version);return valid;
    }
    /* Support the known-good 0.3.0 driver for EFI-only rollback. */
    version=IORegistryEntryCreateCFProperty(service,CFSTR("ThinkBookControlledVersion"),NULL,0);
    int valid=version && CFEqual(version,CFSTR("0.3.0"));
    if(version)CFRelease(version);
    return valid;
}

int main(int argc,char**argv) {
    const char* commands[]={"status","probe","initialize","arm","stop"};
    int command=-1;
    if(argc==2)for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);++i)
        if(strcmp(argv[1],commands[i])==0)command=(int)i;
    if(command<0) {
        fprintf(stderr,"Usage: audioctl status|probe|initialize|arm|stop\n");return 2;
    }
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("CirrusAudioFixup"));
    if(!service) {fprintf(stderr,"ThinkBook audio driver is not loaded.\n");return 1;}
    if(!supported_driver(service)) {
        fprintf(stderr,"Unsupported audio driver version.\n");IOObjectRelease(service);return 1;
    }
    kern_return_t result=KERN_SUCCESS;
    if(command!=0) {
        CFStringRef action=CFStringCreateWithCString(NULL,commands[command],kCFStringEncodingUTF8);
        CFDictionaryRef request=NULL;
        if(action) {
            const void* keys[]={CFSTR("ThinkBookAction")};const void* values[]={action};
            request=CFDictionaryCreate(NULL,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        }
        if(!request)result=kIOReturnNoMemory;
        else result=IORegistryEntrySetCFProperties(service,request);
        if(request)CFRelease(request);
        if(action)CFRelease(action);
    }
    CFTypeRef status=IORegistryEntryCreateCFProperty(service,CFSTR("ThinkBook_Control_Status"),NULL,0);
    if(status && CFGetTypeID(status)==CFStringGetTypeID()) {
        char text[256];
        if(CFStringGetCString(status,text,sizeof(text),kCFStringEncodingUTF8))puts(text);
    }
    if(status)CFRelease(status);
    IOObjectRelease(service);
    if(result!=KERN_SUCCESS) {
        fprintf(stderr,"Audio command failed: 0x%08x%s\n",result,
            result==kIOReturnNotPrivileged?" (administrator privileges required)":"");
        return 1;
    }
    return 0;
}
