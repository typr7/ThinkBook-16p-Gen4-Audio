#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build the ThinkBook audio release from source and verified local inputs."""
from pathlib import Path
import argparse, hashlib, json, plistlib, shutil, subprocess
from Tools.embed_firmware import generate

ROOT=Path(__file__).resolve().parent
def run(args):
    subprocess.run([str(x) for x in args],check=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',required=True,type=Path)
    parser.add_argument('--out',type=Path,default=ROOT/'dist')
    parser.add_argument('--firmware-dir',type=Path,required=True,help='Local OEM files; never distributed by this project')
    parser.add_argument('--plugins',type=Path,required=True,help='PlugIns directory from your working VoodooI2C 2.9.1 bundle')
    args=parser.parse_args();sdk=args.sdk.resolve();out=args.out.resolve()
    if not (sdk/'Headers/IOKit/IOService.h').is_file():parser.error('MacKernelSDK headers missing')
    if not (sdk/'Library/x86_64/libkmod.a').is_file():parser.error('MacKernelSDK libkmod missing')
    out.mkdir(parents=True,exist_ok=True);build=out/'.build';build.mkdir(exist_ok=True)
    generated=build/'generated'
    try:firmware_manifest=generate(args.firmware_dir.resolve(),generated/'Devices/CS35L41/Resources/Firmware.hpp')
    except (ValueError,OSError) as error:parser.error(str(error))
    plugins=args.plugins.resolve()
    plugin_manifest={}
    required={'VoodooGPIO.kext':'org.coolstar.VoodooGPIO','VoodooI2CServices.kext':'com.alexandred.VoodooI2CServices','VoodooInput.kext':'me.kishorprins.VoodooInput'}
    for name,identifier in required.items():
        info=plugins/name/'Contents/Info.plist'
        if not info.is_file():parser.error('Missing plugin: '+name)
        metadata=plistlib.loads(info.read_bytes())
        binary=info.parent/'MacOS'/metadata['CFBundleExecutable']
        if metadata.get('CFBundleIdentifier')!=identifier or not binary.is_file():parser.error('Invalid plugin: '+name)
        plugin_manifest[name]={'bundle_id':identifier,'version':metadata.get('CFBundleVersion'),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
    resource=Path(subprocess.check_output(['clang','-print-resource-dir'],text=True).strip())/'include'
    common=['-target','x86_64-apple-macos13.0','-mkernel','-fapple-kext','-fno-exceptions','-fno-rtti',
        '-fno-common','-fno-builtin','-fno-stack-protector','-mno-red-zone','-mno-mmx','-mno-sse','-mno-sse2',
        '-mcmodel=kernel','-DKERNEL=1','-DKERNEL_PRIVATE=1','-DDRIVER_PRIVATE=1','-DDEBUG=0','-Os',
        '-nostdinc','-I'+str(sdk/'Headers'),'-I'+str(sdk/'Headers/libkern'),'-I'+str(sdk/'Headers/bsd'),
        '-isystem',str(resource),'-Wno-inconsistent-missing-override']
    def bundle(name,executable,identifier,version,sources,includes,standard,info):
        directory=out/name/'Contents';(directory/'MacOS').mkdir(parents=True,exist_ok=True)
        flags=common+['-std='+standard]+['-I'+str(x) for x in includes]
        objects=[]
        for n,source in enumerate(sources):
            obj=build/f'{name}-{n}.o'
            run(['clang++',*flags,'-MMD','-MF',str(obj)+'.d','-c',source,'-o',obj]);objects.append(obj)
        module=build/(name+'.cpp')
        module.write_text('''#include <mach/mach_types.h>
#include <mach/kmod.h>
extern "C" {
extern kern_return_t _start(kmod_info_t*,void*);
extern kern_return_t _stop(kmod_info_t*,void*);
KMOD_EXPLICIT_DECL('''+identifier+',"'+version+'''",_start,_stop)
__attribute__((visibility("hidden"))) kmod_start_func_t* _realmain=nullptr;
__attribute__((visibility("hidden"))) kmod_stop_func_t* _antimain=nullptr;
__attribute__((visibility("hidden"))) int _kext_apple_cc=__APPLE_CC__;
}
''')
        obj=build/(name+'-module.o');run(['clang++',*flags,'-c',module,'-o',obj]);objects.append(obj)
        run(['ld','-arch','x86_64','-kext','-undefined','dynamic_lookup','-no_data_in_code_info',
             '-no_function_starts','-no_compact_unwind','-macos_version_min','13.0','-o',directory/'MacOS'/executable,
             *objects,'-L'+str(sdk/'Library/x86_64'),'-lkmod'])
        shutil.copy2(ROOT/'Resources'/info,directory/'Info.plist')
    vendor=ROOT/'Vendor/VoodooI2C'
    sources=[vendor/path for path in json.loads((ROOT/'Resources/I2C-sources.json').read_text())]
    inputs=ROOT/'Vendor/VoodooInput/VoodooInput'
    bundle('ThinkBookI2C.kext','VoodooI2C','com.alexandred.VoodooI2C','2.9.1',sources,
        [inputs/'VoodooInputMultitouch',inputs],'gnu++11','I2C-Info.plist')
    for name in required:
        destination=out/'ThinkBookI2C.kext/Contents/PlugIns'/name
        if destination.exists():shutil.rmtree(destination)
        shutil.copytree(plugins/name,destination,copy_function=shutil.copyfile,ignore=shutil.ignore_patterns('._*','.DS_Store'))
    bundle('ThinkBookAudio.kext','ThinkBookAudio','local.numw.ThinkBookAudio','1.0.0',
        [ROOT/'Audio/CirrusAudioFixup.cpp'],[generated,ROOT/'Audio'],'c++17','Audio-Info.plist')
    for name,frameworks in [('audioctl',['IOKit','CoreFoundation']),('audio-clock',['AudioToolbox','CoreFoundation'])]:
        command=['clang','-O2','-Wall','-Wextra','-Werror',ROOT/'Service'/(name+'.c'),'-o',out/name]
        for framework in frameworks:command += ['-framework',framework]
        run(command)
    shutil.copy2(ROOT/'Service/start-audio.sh',out/'start-audio.sh')
    shutil.copy2(ROOT/'Resources/local.numw.thinkbook-audio.plist',out/'local.numw.thinkbook-audio.plist')
    (out/'BUILD-INPUTS.json').write_text(json.dumps({'project_version':'1.0.0','oem_files':firmware_manifest,'plugins':plugin_manifest},indent=2)+'\n')
    hashes={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in out.rglob('*') if p.is_file() and '.build' not in p.parts and p.name!='SHA256.json'}
    (out/'SHA256.json').write_text(json.dumps(hashes,indent=2)+'\n')
    print('Release built:',out)
if __name__=='__main__':main()
