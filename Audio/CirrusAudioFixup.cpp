#include "Support/OwnCalibration.hpp"
#include "CirrusAudioFixup.hpp"
#include "Support/ThinkBookOutputPolicy.hpp"
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>

#include "Devices/CS35L41/Resources/Firmware.hpp"
#include "Firmware/WMFW/FirmwareUploader.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

#define super IOService
using namespace cirrus::diagnostics;
OSDefineMetaClassAndStructors(CirrusAudioFixup, IOService)

    class CirrusAudioFixup;

class FixupRegisterIOAdapter : public cirrus::core::RegisterIO {
private:
    CirrusAudioFixup* mFixup;
    AmplifierState& mAmp;

public:
    FixupRegisterIOAdapter(CirrusAudioFixup* fixup, AmplifierState& amp) : mFixup(fixup), mAmp(amp) {}

    bool read(uint32_t address, uint32_t* value) override {
        return mFixup->readRegister(mAmp, address, value, cirrus::diagnostics::TRACE_PROBE);
    }

    bool write(uint32_t address, uint32_t value) override {
        return mFixup->writeRegister(mAmp, address, value, cirrus::diagnostics::TRACE_PROBE);
    }

    bool updateBits(uint32_t address, uint32_t mask, uint32_t value) override {
        return mFixup->updateRegisterBits(mAmp, address, mask, value, cirrus::diagnostics::TRACE_PROBE);
    }

    bool pollBit(uint32_t address, uint32_t mask, uint32_t expected, uint32_t timeoutMs) override {
        return mFixup->pollRegisterBit(mAmp, address, mask, expected, timeoutMs, cirrus::diagnostics::TRACE_PROBE);
    }

    bool bulkRead(uint32_t address, uint8_t* data, size_t length) override {
        return mFixup->bulkRead(mAmp, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }

    bool bulkWrite(uint32_t address, const uint8_t* data, size_t length) override {
        return mFixup->bulkWrite(mAmp, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }
};

static UInt32 readBE32(const UInt8* data) {
    return (static_cast<UInt32>(data[0]) << 24) | (static_cast<UInt32>(data[1]) << 16) | (static_cast<UInt32>(data[2]) << 8) |
           static_cast<UInt32>(data[3]);
}

static void writeBE32(UInt8* data, UInt32 value) {
    data[0] = static_cast<UInt8>((value >> 24) & 0xFF);
    data[1] = static_cast<UInt8>((value >> 16) & 0xFF);
    data[2] = static_cast<UInt8>((value >> 8) & 0xFF);
    data[3] = static_cast<UInt8>(value & 0xFF);
}

bool gCirrusDebug = false;

bool CirrusAudioFixup::init(OSDictionary* properties) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }
    if (!super::init(properties)) {
        CIRRUS_ERR("super::init failed");
        return false;
    }

    setProperty("CirrusReachedInit", kOSBooleanTrue);
    CIRRUS_LOG("init");
    return true;
}

IOService* CirrusAudioFixup::probe(IOService* provider, SInt32* score) {
    IOService* result = super::probe(provider, score);

    setProperty("CirrusReachedProbe", kOSBooleanTrue);

    CIRRUS_LOG("probe provider=%s class=%s score=%d", provider ? provider->getName() : "null",
               provider ? provider->getMetaClass()->getClassName() : "null", score ? static_cast<int>(*score) : -1);

    if (score) {
        *score += 9000;
    }

    return result;
}

bool CirrusAudioFixup::start(IOService* provider) {
    if (bootArgEnabled("-cirrusoff")) {
        return false;
    }

    setProperty("Author", "Tran Kinh Hoang (hoaugtr)");
    setProperty("CirrusReachedStart", kOSBooleanTrue);
    CIRRUS_LOG("start");

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed");
        return false;
    }

    auto nub=provider && provider->metaCast("VoodooI2CDeviceNub") ? provider : nullptr;
    auto acpiObject=nub?nub->getProperty("acpi-device"):nullptr;
    auto acpi=acpiObject?static_cast<IOACPIPlatformDevice*>(acpiObject->metaCast("IOACPIPlatformDevice")):nullptr;
    auto parent=nub?nub->getParentEntry(gIOServicePlane):nullptr;
    auto controller=parent?parent->metaCast("VoodooI2CControllerDriver"):nullptr;
    auto hid=nub?OSDynamicCast(OSString,nub->getProperty("IOName")):nullptr;
    if(!nub||!acpi||!controller||!hid||!hid->isEqualTo("CSC3551"))return false;
    OSObject* subObject=nullptr;IOReturn subResult=acpi->evaluateObject("_SUB",&subObject);
    auto sub=OSDynamicCast(OSString,subObject);bool identity=subResult==kIOReturnSuccess&&sub&&sub->isEqualTo("17AA38A9");
    if(subObject)subObject->release();if(!identity)return false;
    bool pciOK=false;
    for(IORegistryEntry* ancestor=provider;ancestor;ancestor=ancestor->getParentEntry(gIOServicePlane)){
        auto vd=OSDynamicCast(OSData,ancestor->getProperty("vendor-id"));auto dd=OSDynamicCast(OSData,ancestor->getProperty("device-id"));
        if(vd&&dd&&vd->getLength()>=2&&dd->getLength()>=2){auto v=(const uint8_t*)vd->getBytesNoCopy();auto d=(const uint8_t*)dd->getBytesNoCopy();if(v[0]==0x86&&v[1]==0x80&&d[0]==0xd9&&d[1]==0x51){pciOK=true;break;}}
    }
    if(!pciOK)return false;
    mProvider = provider;

    if (!setupProbeTimer()) return false;
    if (!setupPowerManagement(provider)) {stop(provider);return false;}
    setProperty("ThinkBookAudioVersion","1.0.0");
    setProperty("ThinkBook_Control_Status","READY_MANUAL_NO_AMP_ACCESS");
    setProperty("ThinkBook_Automatic_Boot_Initialization",false);
    registerService();
    return true;
}

void CirrusAudioFixup::fullDriverFlow() {
    CIRRUS_LOG("starting hardware initialization flow");

    for (size_t i = 0; i < mAmpCount; ++i) {
        AmplifierState& amp = mAmps[i];
        if (amp.playbackFaulted)
            continue;
        CIRRUS_LOG("initializing amplifier: %s", amp.name);

        amp.initialized = amp.firmwareValidated = amp.dspAlive = false;
        amp.monitorCount = 0;
        amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
        amp.diagnosticControlCount = 0;
        amp.firmwareIdVersion = 0;
        auto abortInitialization = [&]() {
            amp.playbackFaulted = true;
            amp.initialized = amp.firmwareValidated = false;

            if (amp.present) {
                if (!stopPlayback(amp))
                    recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
                if (!stopDSP(amp))
                    recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            }
            amp.dspAlive = false;
            amp.monitorCount = 0;
            amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
            amp.diagnosticControlCount = 0;
        };

        setDiagnosticStage(amp, STAGE_PROBE);

        if (!initCodec(amp)) {
            CIRRUS_ERR("failed to initialize codec for %s", amp.name);
            if (amp.diagnostic.latestFailure == DIAG_OK)
                recordDiagnosticFailure(amp, DIAG_DEVICE_ID, cirrus::devices::cs35l41::registers::kRegDeviceId,
                                        cirrus::devices::cs35l41::registers::kValDeviceId, amp.deviceId);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_OTP_BOOT);

        setDiagnosticStage(amp, STAGE_ERRATA);
        if (!initializeHardwareErrata(amp)) {
            CIRRUS_ERR("failed to apply hardware errata for %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_ERRATA);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_ERRATA);

        setDiagnosticStage(amp, STAGE_CLOCK);
        if (!applyPLL(amp)) {
            recordDiagnosticFailure(amp, DIAG_PLL_CONFIG, cirrus::devices::cs35l41::registers::kRegPllClockControl);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_CLOCK);

        setDiagnosticStage(amp, STAGE_ASP);
        if (!applyASP(amp)) {
            recordDiagnosticFailure(amp, DIAG_ASP_CONFIG, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_ASP);

        setDiagnosticStage(amp, STAGE_GPIO);
        if (!applyGPIO(amp)) {
            recordDiagnosticFailure(amp, DIAG_GPIO_CONFIG, cirrus::devices::cs35l41::registers::kRegGpioPadControl, 0x02000000);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_GPIO);

        setDiagnosticStage(amp, STAGE_PLATFORM);
        if (!configureHardware(amp)) {
            CIRRUS_ERR("platform hardware configuration failed for %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_PLATFORM_CONFIG);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_PLATFORM);

        {
            setDiagnosticStage(amp, STAGE_FIRMWARE_DISCOVERY);
            discoverFirmware(amp);
            if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
                recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
                abortInitialization();
                continue;
            }
            markDiagnosticSuccess(amp, STAGE_FIRMWARE_DISCOVERY);
            setDiagnosticStage(amp, STAGE_FIRMWARE_UPLOAD);
            initializeFirmware(amp);
            if (amp.playbackFaulted || !amp.firmwareValidated || !amp.dspAlive || amp.monitorCount != 1) {
                if (amp.diagnostic.latestFailure == DIAG_OK)
                    recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
                abortInitialization();
                continue;
            }
            markDiagnosticSuccess(amp, STAGE_DSP_BOOT);
        }

        setDiagnosticStage(amp, STAGE_IDLE_VERIFY);
        if (!powerUpAmplifier(amp)) {
            recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK);
            abortInitialization();
            continue;
        }
        if (!verifyIdleConfiguration(amp)) {
            CIRRUS_ERR("idle hardware verification failed for %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_IDLE_INVARIANT);
            abortInitialization();
            continue;
        }
        if ( !verifyDSPAlive(amp)) {
            recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
            abortInitialization();
            continue;
        }
        markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);

        amp.initialized = true;

        CIRRUS_LOG("amplifier %s initialized successfully (dspAlive=%d)", amp.name, amp.dspAlive);
    }

    publishDriverVerdict();
    CIRRUS_LOG("hardware initialization flow complete");
}

bool CirrusAudioFixup::setupPowerManagement(IOService* provider) {
    PMinit();
    mPMInitialized = true;
    provider->joinPMtree(this);
    mPowerStates[0].version = 1;
    mPowerStates[1].version = 1;
    mPowerStates[1].capabilityFlags = kIOPMPowerOn | kIOPMDeviceUsable;
    mPowerStates[1].outputPowerCharacter = kIOPMPowerOn;
    mPowerStates[1].inputPowerRequirement = kIOPMPowerOn;
    return registerPowerDriver(this, mPowerStates, 2) == kIOReturnSuccess;
}

IOReturn CirrusAudioFixup::lifecycleAction(OSObject* owner, void* operation, void*, void*, void*) {
    auto* self = OSDynamicCast(CirrusAudioFixup, owner);
    if (!self)
        return kIOReturnBadArgument;
    const uintptr_t action = reinterpret_cast<uintptr_t>(operation);
    if (action == 2) {
        self->handlePowerChange(false);
        self->mStopping = true;
    } else if (!self->mStopping) {
        self->handlePowerChange(action != 0);
    }
    return kIOReturnSuccess;
}

IOReturn CirrusAudioFixup::setPowerState(unsigned long state, IOService* device) {
    if (device != this || state > 1)
        return IOPMAckImplied;
    if (mCommandGate) {
        IOReturn result = mCommandGate->runAction(lifecycleAction, reinterpret_cast<void*>(state));
        if (result != kIOReturnSuccess)
            CIRRUS_ERR("PM gate failed: 0x%08X", result);
    }
    return IOPMAckImplied;
}

void CirrusAudioFixup::handlePowerChange(bool powered) {
    if (mStopping || powered == mPowerAvailable)
        return;
    if (!powered) {
        if (mProbeTimer)
            mProbeTimer->cancelTimeout();

        if (mManualArmed && mOwnCalibrationValidated) {
            for (auto& amp : mAmps) {
                if (!amp.present)
                    continue;
                bool idle = stopPlayback(amp);
                bool halted = stopDSP(amp);
                if (!idle || !halted)
                    amp.playbackFaulted = true;
                amp.initialized = false;
                amp.firmwareValidated = false;
                amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
            }
        }
        mManualArmed = false;
        mOwnCalibrationValidated = false;
        mPowerAvailable = false;
        mHdaState.streamActive = mHdaState.observed = mHdaState.converterPrepared = false;
        mHdaState.lastDescriptor = 0xFF;
        mHdaState.lastStreamTag = 0;
        mHdaState.lastFormat = 0;
        setProperty("Cirrus_PM_Powered", uint64_t(0), 32);
    } else {
        mPowerAvailable = true;
        mNeedsReinitialization = false;
        mHdaState.topologyLogged = false;
        setProperty("Cirrus_PM_Powered", uint64_t(1), 32);
        setProperty("ThinkBook_Control_Status","READY_MANUAL_REARM_AFTER_WAKE");
    }
    publishDriverVerdict();
}

void CirrusAudioFixup::stop(IOService* provider) {
    if (mCommandGate) {
        mCommandGate->runAction(lifecycleAction, reinterpret_cast<void*>(uintptr_t(2)));
    } else {
        handlePowerChange(false);
        mStopping = true;
    }
    if (mPMInitialized) {
        PMstop();
        mPMInitialized = false;
    }
    if (mProbeTimer && mWorkLoop) {
        mProbeTimer->cancelTimeout();
        mWorkLoop->removeEventSource(mProbeTimer);
    }
    OSSafeReleaseNULL(mProbeTimer);
    if (mCommandGate && mWorkLoop)
        mWorkLoop->removeEventSource(mCommandGate);
    OSSafeReleaseNULL(mCommandGate);
    OSSafeReleaseNULL(mWorkLoop);
    OSSafeReleaseNULL(mHdaMapCache);
    OSSafeReleaseNULL(mHdaControllerCache);
    OSSafeReleaseNULL(mSpeakerSelectorCache);
    mProvider = nullptr;
    super::stop(provider);
}

void CirrusAudioFixup::free() { super::free(); }

bool CirrusAudioFixup::bootArgEnabled(const char* name) {
    if(strcmp(name,"-cirrusoff")!=0)return false;

    char val[16] = {};
    if (PE_parse_boot_argn(name, val, sizeof(val))) {
        if (val[0] == '0' && val[1] == '\0') {
            return false;
        }
        return true;
    }
    return false;
}

bool CirrusAudioFixup::setupProbeTimer() {
    mWorkLoop = IOWorkLoop::workLoop();

    if (!mWorkLoop) {
        return false;
    }

    mProbeTimer = IOTimerEventSource::timerEventSource(this, probeTimerFired);
    if (!mProbeTimer) {
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }

    if (mWorkLoop->addEventSource(mProbeTimer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(mProbeTimer);
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }

    mCommandGate = IOCommandGate::commandGate(this);
    if (!mCommandGate || mWorkLoop->addEventSource(mCommandGate) != kIOReturnSuccess) {
        OSSafeReleaseNULL(mCommandGate);
        mWorkLoop->removeEventSource(mProbeTimer);
        OSSafeReleaseNULL(mProbeTimer);
        OSSafeReleaseNULL(mWorkLoop);
        return false;
    }
    setProperty("CirrusTimerCreated", kOSBooleanTrue);
    return true;
}

void CirrusAudioFixup::initializeFirmware(AmplifierState& amp) {
    CIRRUS_LOG("starting full initialization for amplifier: %s", amp.name);
    amp.firmwareValidated = false;
    amp.dspAlive = false;
    amp.monitorCount = 0;

    if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
        CIRRUS_ERR("firmware or tuning data missing for %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_MISSING, 0, 1, 0);
        return;
    }

    bool wmfwUploaded = false;
    bool coefficientsUploaded = false;

    if (!stopDSP(amp)) {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        amp.playbackFaulted = true;
        return;
    }
    amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
    amp.diagnosticControlCount = 0;

    FirmwareImage* image = (FirmwareImage*)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_UPLOAD, 0, (UInt32)sizeof(FirmwareImage), 0, kIOReturnNoMemory, false);
        return;
    }

    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed for %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    CIRRUS_LOG("wmfw parsing successful on %s. fw id: 0x%06X, expected algs: %u", amp.name, image->firmwareId, image->algorithmTotal);

    MappedImage* wmfwMapped = (MappedImage*)IOMalloc(sizeof(MappedImage));
    if (wmfwMapped) {
        if (CirrusFirmwareMapper::mapFirmwareImage(*image, *wmfwMapped)) {
            CIRRUS_LOG("mapping wmfw image successful on %s, starting upload", amp.name);
            UploadSession session;
            FixupRegisterIOAdapter io(this, amp);
            wmfwUploaded = CirrusFirmwareScheduler::run(amp.name, io, *wmfwMapped, session);
        } else {
            CIRRUS_ERR("failed to map wmfw image on %s", amp.name);
            recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        }
        IOFree(wmfwMapped, sizeof(MappedImage));
    }
    if (!wmfwUploaded) {
        CIRRUS_ERR("wmfw upload failed on %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_UPLOAD);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    CIRRUS_LOG("parsing dsp algorithms for %s", amp.name);
    if (!parseDSPAlgorithms(amp, *image)) {
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    WMFWControlRef stateRef{}, heartbeatRef{};
    if (!CirrusFirmwareParser::findControl(image, "HALO_STATE", stateRef) ||
        !CirrusFirmwareParser::findControl(image, "HALO_HEARTBEAT", heartbeatRef) ||
        (stateRef.algorithm->id & 0x00FFFFFF) != (image->firmwareId & 0x00FFFFFF) ||
        (heartbeatRef.algorithm->id & 0x00FFFFFF) != (image->firmwareId & 0x00FFFFFF) || stateRef.control->type != WMFW_ADSP2_XM ||
        stateRef.control->len != 4 || heartbeatRef.control->type != WMFW_ADSP2_XM || heartbeatRef.control->len != 4 ||
        !CirrusFirmwareParser::resolveControl(*image, stateRef, amp.haloStateRegister) ||
        !CirrusFirmwareParser::resolveControl(*image, heartbeatRef, amp.haloHeartbeatRegister)) {
        amp.haloStateRegister = amp.haloHeartbeatRegister = 0;
        recordDiagnosticFailure(amp, DIAG_FIRMWARE_PARSE);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    CIRRUS_LOG("HALO controls on %s: state=0x%08X heartbeat=0x%08X firmware=0x%06X", amp.name, amp.haloStateRegister,
               amp.haloHeartbeatRegister, image->firmwareId);
    amp.firmwareIdVersion = image->haloFirmwareVersion;
    CIRRUS_LOG("HALO firmware ID version on %s: 0x%06X", amp.name, amp.firmwareIdVersion);
    char fwIdVersionProp[64];
    snprintf(fwIdVersionProp, sizeof(fwIdVersionProp), "Cirrus_FW_ID_Version_%s", amp.name);
    setProperty(fwIdVersionProp, (uint64_t)amp.firmwareIdVersion, 32);

    if (CirrusFirmwareParser::parseBIN(amp.binData, amp.binSize, image)) {
        CIRRUS_LOG("bin parsing successful on %s, found %u coefficient blocks", amp.name, image->coefficientCount);
        MappedImage* coeffMapped = (MappedImage*)IOMalloc(sizeof(MappedImage));
        if (coeffMapped) {
            if (CirrusFirmwareMapper::mapCoefficients(*image, *coeffMapped) && coeffMapped->regionCount > 0) {
                CIRRUS_LOG("starting coefficient file upload on %s", amp.name);
                UploadSession session;
                FixupRegisterIOAdapter io(this, amp);
                coefficientsUploaded = CirrusFirmwareScheduler::run(amp.name, io, *coeffMapped, session);
            } else {
                CIRRUS_ERR("coefficient mapping produced no uploadable regions on %s", amp.name);
                recordDiagnosticFailure(amp, DIAG_COEFFICIENT_PARSE);
            }
            IOFree(coeffMapped, sizeof(MappedImage));
        }
    } else {
        CIRRUS_ERR("bin file parsing failed on %s", amp.name);
        recordDiagnosticFailure(amp, DIAG_COEFFICIENT_PARSE);
    }

    if (!coefficientsUploaded) {
        CIRRUS_ERR("coefficient upload failed on %s; keeping DSP stopped and output disabled", amp.name);
        if (amp.diagnostic.latestFailure != DIAG_COEFFICIENT_PARSE)
            recordDiagnosticFailure(amp, DIAG_COEFFICIENT_UPLOAD);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!applyCalibration(amp, image)) {
        CIRRUS_ERR("calibration failed on %s; keeping DSP stopped", amp.name);
        recordDiagnosticFailure(amp, DIAG_CALIBRATION);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    amp.firmwareValidated = true;

    CIRRUS_LOG("bringing up dsp controller on %s", amp.name);
    bool booted = bringupDSP(amp);

    if (booted && verifyDSPAlive(amp)) {
        CIRRUS_LOG("dsp is successfully verified alive on %s", amp.name);
        amp.dspAlive = true;
        amp.monitorCount = 1;
    } else {
        CIRRUS_ERR("dsp bringup failed or dsp is unresponsive on %s; disabling DSP mode", amp.name);
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        amp.firmwareValidated = false;
        amp.dspAlive = false;
        amp.monitorCount = 0;
        stopDSP(amp);
    }

    IOFree(image, sizeof(FirmwareImage));
    CIRRUS_LOG("full firmware init complete for amplifier %s", amp.name);
}

bool CirrusAudioFixup::applyCalibration(AmplifierState& amp, const FirmwareImage* image) {
    const char* fields[] = {"Status", "R0", "Ambient", "Valid", "Checksum"};
    for (const char* field : fields) {
        char property[80];
        snprintf(property, sizeof(property), "Cirrus_Calibration_%s_%s", field, amp.name);
        removeProperty(property);
    }
    if (!image)
        return false;

    uint32_t addrAmbient = 0, addrR = 0, addrStatus = 0, addrChecksum = 0;
    WMFWControlRef refAmbient{}, refR{}, refStatus{}, refChecksum{};
    bool resolved =
        CirrusFirmwareParser::findControl(image, "CAL_AMBIENT", refAmbient) && refAmbient.algorithm->id == 0xCD &&
        refAmbient.control->type == WMFW_ADSP2_XM && refAmbient.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refAmbient, addrAmbient) && CirrusFirmwareParser::findControl(image, "CAL_R", refR) &&
        refR.algorithm->id == 0xCD && refR.control->type == WMFW_ADSP2_XM && refR.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refR, addrR) && CirrusFirmwareParser::findControl(image, "CAL_STATUS", refStatus) &&
        refStatus.algorithm->id == 0xCD && refStatus.control->type == WMFW_ADSP2_XM && refStatus.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refStatus, addrStatus) &&
        CirrusFirmwareParser::findControl(image, "CAL_CHECKSUM", refChecksum) && refChecksum.algorithm->id == 0xCD &&
        refChecksum.control->type == WMFW_ADSP2_XM && refChecksum.control->len == 4 &&
        CirrusFirmwareParser::resolveControl(*image, refChecksum, addrChecksum);

    if (!resolved) {
        CIRRUS_ERR("failed to resolve all 4 calibration controls in algorithm 0xCD on %s", amp.name);
        char calProp[80];
        snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
        OSString* calVal = OSString::withCString("CONTROL_RESOLUTION_FAILED");
        if (calVal) {
            setProperty(calProp, calVal);
            calVal->release();
        }
        return false;
    }

    if (!mOwnCalibrationValidated) return false;
    OwnAmpCalibration calibration{};
    if (!readOwnCalibration(amp, calibration)) return false;
    const int32_t ambientVal = calibration.ambient;
    const uint32_t statusVal = calibration.status;
    const uint32_t r0Val = calibration.resistance;
    const uint32_t checksumVal = r0Val + 1;
    const char* sourceStr = "APPLIED_FROM_EFI_VARIABLE";

    bool ok = writeRegister(amp, addrAmbient, (uint32_t)ambientVal, TRACE_FIRMWARE) && writeRegister(amp, addrR, r0Val, TRACE_FIRMWARE) &&
              writeRegister(amp, addrStatus, statusVal, TRACE_FIRMWARE) && writeRegister(amp, addrChecksum, checksumVal, TRACE_FIRMWARE);

    uint32_t rbAmbient = 0, rbR = 0, rbStatus = 0, rbChecksum = 0;
    bool verified = ok && readRegister(amp, addrAmbient, &rbAmbient, TRACE_FIRMWARE) && readRegister(amp, addrR, &rbR, TRACE_FIRMWARE) &&
                    readRegister(amp, addrStatus, &rbStatus, TRACE_FIRMWARE) &&
                    readRegister(amp, addrChecksum, &rbChecksum, TRACE_FIRMWARE) && rbAmbient == (uint32_t)ambientVal && rbR == r0Val &&
                    rbStatus == statusVal && rbChecksum == checksumVal;

    char calProp[80];
    snprintf(calProp, sizeof(calProp), "Cirrus_Calibration_Status_%s", amp.name);
    OSString* calVal = OSString::withCString(verified ? sourceStr : "VERIFY_FAILED");
    if (calVal) {
        setProperty(calProp, calVal);
        calVal->release();
    }

    if (!verified) {
        CIRRUS_ERR("calibration verification failed on %s: ok=%d verified=%d", amp.name, ok, verified);
        return false;
    }

    return true;
}

void CirrusAudioFixup::probeTimerFired(OSObject* owner, IOTimerEventSource*) {
    auto* self=OSDynamicCast(CirrusAudioFixup,owner);
    if(self && !self->mStopping && self->mPowerAvailable && self->mManualArmed)self->runBackgroundMonitor();
}

size_t CirrusAudioFixup::detectAmplifiers() {
    for (size_t i = 0; i < kMaxAmps; ++i) {
        mAmps[i].present = false;
    }

    const uint8_t candidateAddresses[kMaxAmps] = {cirrus::devices::cs35l41::registers::kI2cAddressLeft,
                                                  cirrus::devices::cs35l41::registers::kI2cAddressRight, 0x42, 0x43};
    const char* candidateNames[kMaxAmps] = {"left", "right", "top_left", "top_right"};

    size_t detected = 0;
    mProbingAmplifiers = true;
    for (size_t i = 0; i < 2; ++i) {
        uint8_t addr = candidateAddresses[i];
        uint8_t writeBuf[4] = {0x00, 0x00, 0x00, 0x00};
        uint8_t readBuf[4] = {0};

        bool ok = transferToAddress(addr, writeBuf, sizeof(writeBuf), readBuf, sizeof(readBuf));
        uint32_t devId = 0;
        if (ok) {
            devId = (static_cast<uint32_t>(readBuf[0]) << 24) | (static_cast<uint32_t>(readBuf[1]) << 16) |
                    (static_cast<uint32_t>(readBuf[2]) << 8) | static_cast<uint32_t>(readBuf[3]);
        }

        if (devId == cirrus::devices::cs35l41::registers::kValDeviceId || devId == 0x00035A40) {
            mAmps[detected].name = candidateNames[i];
            mAmps[detected].address = addr;
            mAmps[detected].present = true;
            mAmps[detected].deviceId = devId;
            detected++;
            CIRRUS_LOG("detected amplifier %s at address 0x%02X (DEVID=0x%08X)", candidateNames[i], addr, devId);
        }
    }
    mProbingAmplifiers = false;

    mAmpCount = detected;
    setProperty("Cirrus_Detected_Amplifiers", (uint64_t)mAmpCount, 32);
    CIRRUS_LOG("dynamic amplifier detection complete: %lu speaker(s) active", mAmpCount);
    return mAmpCount;
}

bool CirrusAudioFixup::supportedHdaFormat(uint16_t format) {
    const uint32_t baseRate = (format & 0x4000) ? 44100 : 48000;
    const uint32_t multiplier = ((format >> 11) & 7) + 1;
    const uint32_t divisor = ((format >> 8) & 7) + 1;
    const uint32_t sampleSize = (format >> 4) & 7;

    return !(format & 0x8080) && multiplier <= 4 && baseRate * multiplier == 48000 * divisor && (format & 15) == 1 && sampleSize >= 1 &&
           sampleSize <= 3;
}

bool CirrusAudioFixup::synchronizeHdaStream() {
    const bool wasPrepared = mHdaState.converterPrepared;
    auto publishStatus = [&](const char* text) {
        OSString* status = OSString::withCString(text);
        if (status) {
            setProperty("Cirrus_HDA_Status", status);
            status->release();
        }
    };
    mHdaState.observed = false;
    mHdaState.streamActive = mHdaState.converterPrepared = false;
    mHdaState.lastDescriptor = 0xFF;
    mHdaState.lastStreamTag = 0;
    mHdaState.lastFormat = 0;
    if (!mPowerAvailable || mStopping)
        return false;
    IOService* audioCtrl = getAudioController();
    if (!audioCtrl) {
        if (mHdaState.missCount < 20 && ++mHdaState.missCount == 20) {
            for (size_t i = 0; i < mAmpCount; ++i) {
                setDiagnosticStage(mAmps[i], STAGE_HDA_DETECT);
                recordDiagnosticFailure(mAmps[i], DIAG_HDA_CONTROLLER, 0, 1, 0, kIOReturnNotFound, false);
            }
        }
        publishStatus(mHdaState.missCount >= 20 ? "MISSING_AFTER_20_POLLS" : "MISSING");
        return false;
    }
    if (mHdaState.missCount != 0) {
        CIRRUS_LOG("HDA controller discovered after %u missed polls", mHdaState.missCount);
        mHdaState.missCount = 0;
    }

    IOPCIDevice* pciDev = OSDynamicCast(IOPCIDevice, audioCtrl);
    if (!pciDev) {
        publishStatus("NOT_A_PCI_CONTROLLER");
        audioCtrl->release();
        return false;
    }

    if(!mHdaMapCache)mHdaMapCache=pciDev->mapDeviceMemoryWithRegister(0x10);
    IOMemoryMap* map=mHdaMapCache;
    if(map)map->retain();
    if (!map || map->getLength() < 0x80) {
        publishStatus("INVALID_BAR_MAPPING");
        if (map)
            map->release();
        audioCtrl->release();
        return false;
    }

    volatile uint8_t* base = (volatile uint8_t*)map->getVirtualAddress();
    mHdaState.observed = base != nullptr;
    if (!base) {
        publishStatus("INVALID_BAR_ADDRESS");
        map->release();
        audioCtrl->release();
        return false;
    }

    bool outputRunning = false;
    uint8_t activeStream = 0;
    uint16_t activeFormat = 0;
    uint8_t activeDescriptor = 0xFF;
    unsigned runningOutputs = 0;

    uint16_t gcap = *(volatile uint16_t*)(base + 0x00);
    uint32_t gctl = *(volatile uint32_t*)(base + 0x08);
    if (gcap == 0xFFFF || gctl == 0xFFFFFFFF || !(gctl & 1)) {
        publishStatus("CONTROLLER_NOT_READY");
        mHdaState.observed = false;
        map->release();
        audioCtrl->release();
        return false;
    }
    uint8_t inputStreams = (gcap >> 8) & 0x0F;
    uint8_t outputStreams = (gcap >> 12) & 0x0F;
    uint8_t bidirectionalStreams = (gcap >> 3) & 0x1F;
    uint8_t firstOutput = inputStreams;
    uint8_t descriptorCount = inputStreams + outputStreams + bidirectionalStreams;
    if (map->getLength() < 0x80U + uint32_t(descriptorCount) * 0x20U) {
        publishStatus("TRUNCATED_STREAM_DESCRIPTORS");
        mHdaState.observed = false;
        map->release();
        audioCtrl->release();
        return false;
    }

    if (!mHdaState.topologyLogged && pciDev) {
        CIRRUS_LOG("HDA controller %s %04X:%04X GCAP=0x%04X ISS=%u OSS=%u BSS=%u; speaker converter/pin route is unverified",
                   pciDev->getName() ? pciDev->getName() : "unnamed", pciDev->configRead16(kIOPCIConfigVendorID),
                   pciDev->configRead16(kIOPCIConfigDeviceID), gcap, inputStreams, outputStreams, bidirectionalStreams);
        mHdaState.topologyLogged = true;
    }

    for (uint8_t index = firstOutput; index < descriptorCount; ++index) {
        bool fixedOutput = index < (uint8_t)(firstOutput + outputStreams);
        volatile uint8_t* sd = base + 0x80 + (index * 0x20);
        uint32_t sdCtl = *(volatile uint32_t*)(sd + 0x00);
        bool bidirectionalOutput = !fixedOutput && ((sdCtl & (1U << 19)) != 0);
        if ((fixedOutput || bidirectionalOutput) && (sdCtl & 0x00000003) == 0x00000002) {
            uint8_t candidateStream = (sdCtl >> 20) & 0x0F;
            if (candidateStream != 0) {
                activeStream = candidateStream;
                activeFormat = *(volatile uint16_t*)(sd + 0x12);
                activeDescriptor = index;
                outputRunning = true;
                ++runningOutputs;
            }
        }
    }

    if (runningOutputs > 1) {
        publishStatus("AMBIGUOUS_OUTPUT_STREAMS");
        map->release();
        audioCtrl->release();
        return false;
    }
    bool converterPrepared = outputRunning && activeStream != 0 && supportedHdaFormat(activeFormat) && internalSpeakersSelected();
    bool streamActive = converterPrepared;

    if (outputRunning && activeStream != 0 && !supportedHdaFormat(activeFormat)) {
        for (size_t i = 0; i < mAmpCount; ++i) {
            setDiagnosticStage(mAmps[i], STAGE_HDA_DETECT);
            recordDiagnosticFailure(mAmps[i], DIAG_HDA_STREAM_FORMAT, activeDescriptor, 1, activeFormat, kIOReturnSuccess, false);
        }
    }

    if (converterPrepared && !wasPrepared) {
        CIRRUS_LOG("HDA output stream observed: descriptor=%u stream=%u format=0x%04X; speaker route unverified", activeDescriptor,
                   activeStream, activeFormat);
    }

    if (!converterPrepared && wasPrepared) {
        CIRRUS_LOG("HDA playback cleanup: no unique compatible running output stream");
    }
    publishStatus(converterPrepared ? "RUNNING_ROUTE_UNCONFIRMED" : outputRunning ? "UNSUPPORTED_STREAM_FORMAT" : "IDLE");
    mHdaState.converterPrepared = converterPrepared;
    mHdaState.streamActive = streamActive;
    mHdaState.lastDescriptor = activeDescriptor;
    mHdaState.lastStreamTag = activeStream;
    mHdaState.lastFormat = activeFormat;
    map->release();
    audioCtrl->release();
    return streamActive;
}

bool CirrusAudioFixup::checkProtectionStatus(AmplifierState& amp) {
    uint32_t status = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kIrq1Status1Register, &status, TRACE_PLAYBACK))
        return false;
    char prop[80];
    snprintf(prop, sizeof(prop), "Cirrus_Protection_Fault_%s", amp.name);
    if (status & cirrus::devices::cs35l41::registers::kMaskProtectionFault) {
        const char* faultReason = "UNKNOWN_FAULT";
        if (status & 0x00000080) {
            faultReason = "CRITICAL_OVERTEMPERATURE";
        } else if (status & 0x80000000) {
            faultReason = "SPEAKER_SHORT_CIRCUIT";
        } else if (status & 0x00020000) {
            faultReason = "BOOST_OVERVOLTAGE";
        } else if (status & 0x00008000) {
            faultReason = "BOOST_SHORT_CIRCUIT";
        } else if (status & 0x00000040) {
            faultReason = "BOOST_PEAK_CURRENT_LIMIT";
        } else if (status & 0x00000100) {
            faultReason = "TEMPERATURE_WARNING";
        }
        OSString* faultVal = OSString::withCString(faultReason);
        if (faultVal) {
            setProperty(prop, faultVal);
            faultVal->release();
        }
        recordDiagnosticFailure(amp, DIAG_AMP_PROTECTION, cirrus::devices::cs35l41::registers::kIrq1Status1Register, 0, status);
        return false;
    }
    OSString* okVal = OSString::withCString("OK");
    if (okVal) {
        setProperty(prop, okVal);
        okVal->release();
    }
    return true;
}

bool CirrusAudioFixup::stopPlayback(AmplifierState& amp) {
    ++amp.cleanupAttempts;
    setDiagnosticStage(amp, STAGE_PLAYBACK_CLEANUP);
    amp.playbackStableCount = 0;
    bool ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0, TRACE_PLAYBACK);
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678, TRACE_PLAYBACK) && ok;
    uint32_t before = 0;
    bool known = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &before, TRACE_PLAYBACK);
    bool needPdn = !known || (before & 1);
    ok = known && ok;
    bool unlocked = unlockTestKey(amp);
    ok = unlocked && ok;
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00585941, TRACE_PLAYBACK) && ok;
    if (needPdn)
        ok = writeRegister(amp, 0x00010010, 0x00800000, TRACE_PLAYBACK) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl1, 1, 0, TRACE_PLAYBACK) && ok;
    if (unlocked)
        ok = writeRegister(amp, 0x0000742C, 0x00000009, TRACE_PLAYBACK) && ok;
    bool pdnDone = !needPdn;
    uint32_t irq = 0;
    for (unsigned i = 0; needPdn && i < 100; ++i) {
        if (!readRegister(amp, 0x00010010, &irq, TRACE_PLAYBACK))
            break;
        if (irq & 0x00800000) {
            pdnDone = true;
            ok = writeRegister(amp, 0x00010010, 0x00800000, TRACE_PLAYBACK) && ok;
            break;
        }
        IODelay(1000);
    }
    if (!pdnDone)
        recordDiagnosticFailure(amp, DIAG_POWER_DOWN_TIMEOUT, 0x00010010, 0x00800000, irq);
    if (unlocked)
        ok = writeRegister(amp, 0x00007438, 0x00580941, TRACE_PLAYBACK) && ok;
    ok = lockTestKey(amp) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegGpio1Control1, 0x00000001, TRACE_PLAYBACK) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 1, 0, TRACE_PLAYBACK) && ok;
    if (amp.dspAlive) {
        // Backport upstream 1.1.x: Linux defines commands 0..7 only.
        // After muted power-down, PAUSE is the supported shutdown command.
        bool paused = sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxPause,
                                         cirrus::devices::cs35l41::registers::kStatusMailboxPaused);
        ok = paused && ok;
        if (!paused) {
            amp.firmwareValidated = false;
            stopDSP(amp);
        }
    }
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2,
                            0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 0, TRACE_PLAYBACK) &&
         ok;
    uint32_t pwr1 = 0, pwr2 = 0, volume = 0, gain = 0;
    bool readable = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &pwr1, TRACE_PLAYBACK) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &pwr2, TRACE_PLAYBACK) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &volume, TRACE_PLAYBACK) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &gain, TRACE_PLAYBACK);
    bool safe = ok && pdnDone && readable && !(pwr1 & 1) &&
                !(pwr2 & (0x00003001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) && volume == 0x0000A678 && gain == 0;
    amp.playbackActive = !safe;
    if (!safe) {
        amp.playbackFaulted = true;
        recordDiagnosticFailure(amp, DIAG_IDLE_ROLLBACK, cirrus::devices::cs35l41::registers::kPowerControl2, 0, pwr2);
    } else if (!amp.playbackFaulted) {
        amp.cleanupAttempts = 0;
        markDiagnosticSuccess(amp, STAGE_SAFE_IDLE);
    }
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Playback_Verdict_%s", amp.name);
    OSString* verdict = OSString::withCString(!safe                 ? "CLEANUP_UNVERIFIED"
                                              : amp.playbackFaulted ? "FAULT_LATCHED_OUTPUT_DISABLED"
                                                                    : "SAFE_IDLE_VERIFIED");
    if (verdict) {
        setProperty(property, verdict);
        verdict->release();
    }
    publishDriverVerdict();
    if (safe) snapshotDiagnostics(amp, "STOPPED VERIFIED");
    return safe;
}

void CirrusAudioFixup::runBackgroundMonitor() {
    if (mStopping || !mPowerAvailable || mNeedsReinitialization || !mManualArmed)
        return;
    bool hdaStreamActive = synchronizeHdaStream();
    IOSleep(5);
    bool hasAudio = mHdaState.observed && hdaStreamActive;
    for (size_t i = 0; i < mAmpCount; ++i) {
        AmplifierState& amp = mAmps[i];
        if (!amp.present)
            continue;
        if (amp.playbackFaulted) {
            if (amp.playbackActive && amp.cleanupAttempts < 3)
                stopPlayback(amp);
            // First ThinkBook trial: latch faults until a new initialization.
            continue;
        }

        // A stopped HDA stream legitimately removes BCLK before the next amp poll.
        // Clean up before judging PLL health; do not latch a normal playback stop.
        if (amp.playbackActive && mHdaState.observed && !hdaStreamActive) {
            stopPlayback(amp);
            continue;
        }

        if (amp.initialized && !amp.playbackActive && mHdaState.observed && !hdaStreamActive) {
            amp.playbackStableCount = 0;
            amp.pllRetryCooldown = 0;
            continue;
        }

        uint32_t pwrCtrl1 = 0;
        uint32_t pllLockSts = 0;
        uint32_t mbox2 = 0;
        uint32_t timestamp = 0;

        if (!readRegister(amp, 0x00002014, &pwrCtrl1, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        bool pllReadable = readRegister(amp, 0x00010098, &pllLockSts, TRACE_DUMP);
        if (!pllReadable) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }
        bool globalEn = (pwrCtrl1 & 0x01) != 0;
        bool pllLock = pllReadable && (pllLockSts & 0x00000002) != 0;

        if (!readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (!amp.initialized) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (!checkProtectionStatus(amp)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (!readRegister(amp, 0x025C0800, &timestamp, TRACE_DUMP)) {
            amp.playbackFaulted = true;
            stopPlayback(amp);
            continue;
        }

        if (amp.playbackActive && hasAudio) {
            const bool dspMode = amp.monitorCount >= 1;
            uint32_t pwr2 = 0, core = 0, halo = 0;
            bool powerReadable = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &pwr2, TRACE_PLAYBACK);
            bool dspHealthy = false;
            {
                dspHealthy = amp.dspAlive && amp.firmwareValidated && amp.haloStateRegister &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core, TRACE_PLAYBACK) &&
                             readRegister(amp, amp.haloStateRegister, &halo, TRACE_PLAYBACK) &&
                             (core & (cirrus::devices::cs35l41::registers::kValHaloCoreEnable |
                                      cirrus::devices::cs35l41::registers::kValHaloCoreReset)) ==
                                 cirrus::devices::cs35l41::registers::kValHaloCoreEnable &&
                             halo == 2 && mbox2 == cirrus::devices::cs35l41::registers::kStatusMailboxRunning;
            }
            // Recheck the HDA stream if it stopped between the two observations.
            if (!pllLock && !synchronizeHdaStream() && mHdaState.observed) {
                stopPlayback(amp);
                continue;
            }
            if (!globalEn || !pllLock || !powerReadable ||
                (pwr2 & (0x3001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) != (dspMode ? 0x3001U : 1U) || !dspHealthy) {
                CIRRUS_ERR("ACTIVE_STATE_LOST amp=%s pwr1=0x%08X pwr2=0x%08X pll=%d mbox=%u core=0x%08X halo=%u", amp.name, pwrCtrl1, pwr2,
                           pllLock, mbox2, core, halo);
                if (!pllLock) {
                    recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, 2, pllLockSts);
                    amp.pllRetryCooldown = 4;
                } else {
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, cirrus::devices::cs35l41::registers::kPowerControl2,
                                            dspMode ? 0x3001U : 1U, pwr2);
                }
                amp.playbackFaulted = true;
                stopPlayback(amp);
                continue;
            }
        }

        bool playbackTransition = hasAudio != amp.playbackActive;
        bool heartbeat = (++amp.monitorLogCountdown >= (hasAudio ? 50U : 40U));
        if (playbackTransition || heartbeat) {
            uint32_t aspEnables = 0, spRate = 0, spFmt = 0, dacSrc = 0, ampVol = 0;
            bool snapshotReadable =
                readRegister(amp, 0x00004800, &aspEnables, TRACE_DUMP) && readRegister(amp, 0x00004804, &spRate, TRACE_DUMP) &&
                readRegister(amp, 0x00004808, &spFmt, TRACE_DUMP) && readRegister(amp, 0x00004C00, &dacSrc, TRACE_DUMP) &&
                readRegister(amp, 0x00006000, &ampVol, TRACE_DUMP);
            if (!snapshotReadable) {
                amp.playbackFaulted = true;
                stopPlayback(amp);
                continue;
            }

            CIRRUS_LOG("background monitor %s: global_en=%d pll_lock=%d mbox2=0x%08X ts=0x%08X->0x%08X audio=%d active=%d "
                       "hda_sd=%u tag=%u fmt=0x%04X sp_en=0x%08X sp_rate=0x%08X sp_fmt=0x%08X dac_src=0x%08X vol=0x%08X mode=%s",
                       amp.name, globalEn, pllLock, mbox2, amp.lastTimestamp, timestamp, hasAudio, amp.playbackActive,
                       mHdaState.lastDescriptor, mHdaState.lastStreamTag, mHdaState.lastFormat, aspEnables, spRate, spFmt, dacSrc, ampVol,
                       (amp.monitorCount >= 1) ? "DSP" : "BYPASS");
            if (heartbeat && amp.playbackActive) snapshotDiagnostics(amp, "PLAYBACK TELEMETRY");
            amp.monitorLogCountdown = 0;
        }

        amp.lastTimestamp = timestamp;

        if (hasAudio && !amp.playbackActive) {
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 1) {
                const uint32_t startStreamTag = mHdaState.lastStreamTag;
                const uint32_t startStreamFormat = mHdaState.lastFormat;
                const bool dspMode = true;
                if ((!dspMode && true) || (dspMode && (!amp.dspAlive || !amp.firmwareValidated))) {
                    amp.playbackFaulted = true;
                    stopPlayback(amp);
                    continue;
                }
                bool dspCommandOk = true;
                bool sequenceOk = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678);
                sequenceOk = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0) && sequenceOk;
                auto checkedWrite = [&](uint32_t reg, uint32_t value) {
                    if (sequenceOk)
                        sequenceOk = writeRegister(amp, reg, value, TRACE_PLAYBACK);
                };
                auto abortStart = [&]() {
                    amp.playbackFaulted = true;
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT);
                    stopPlayback(amp);
                };
                setDiagnosticStage(amp, STAGE_PLAYBACK_OPEN);
                CIRRUS_LOG("Background Monitor: Playback STARTED on %s (mode=%s) — enabling output path", amp.name,
                           dspMode ? "DSP" : "BYPASS");
                amp.playbackStableCount = 0;

                {
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegPllClockControl, 0x00000430);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDspClockControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegGlobalClockControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010000);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortRateControl, 0x00000021);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortFormat, 0x20200200);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000003);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortTxWordLength, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegSerialPortRxWordLength, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x00000032);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx1Source, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx2Source, 0x00000019);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000028);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000029);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000008);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000029);
                    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1Rx6Source, 0x00000029);
                    sequenceOk =
                        sequenceOk && updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2, 0x00003000, 0x00003000);
                    dspCommandOk = sequenceOk && sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxResume,
                                                                    cirrus::devices::cs35l41::registers::kStatusMailboxRunning);
                    sequenceOk = sequenceOk && dspCommandOk;
                }

                if (!sequenceOk || !updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl2,
                                                       1 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 1)) {
                    abortStart();
                    continue;
                }

                checkedWrite(cirrus::devices::cs35l41::registers::kRegGpio1Control1, 0x00008001);
                checkedWrite(0x00010010, 0x01000000);

                setDiagnosticStage(amp, STAGE_PLAYBACK_PREPARE);
                checkedWrite(0x00000040, 0x00000055);
                checkedWrite(0x00000040, 0x000000AA);
                checkedWrite(0x0000742C, 0x0000000F);
                checkedWrite(0x0000742C, 0x00000079);
                checkedWrite(0x00007438, 0x00585941);

                sequenceOk = sequenceOk && updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kPowerControl1, 1, 1);
                if (!sequenceOk) {
                    abortStart();
                    continue;
                }

                uint32_t irq1Sts = 0;
                int pupTimeout = 100;
                int elapsedPup = 0;
                while (pupTimeout > 0) {
                    if (!readRegister(amp, 0x00010010, &irq1Sts)) {
                        sequenceOk = false;
                        break;
                    }
                    if (irq1Sts & 0x01000000)
                        break;
                    IODelay(1000);
                    elapsedPup++;
                    pupTimeout--;
                }
                bool pupDone = (irq1Sts & 0x01000000) != 0;
                if (pupDone) {
                    CIRRUS_LOG("Background Monitor: PUP_DONE observed on %s after %d ms", amp.name, elapsedPup);
                    checkedWrite(0x00010010, 0x01000000);
                } else {
                    CIRRUS_ERR("Background Monitor: PUP_DONE NOT observed on %s within 100ms (irq1=0x%08X)", amp.name, irq1Sts);
                    recordDiagnosticFailure(amp, DIAG_POWER_UP_TIMEOUT, 0x00010010, 0x01000000, irq1Sts);
                }

                if (!sequenceOk || !pupDone) {
                    abortStart();
                    continue;
                }
                {
                    if (amp.firmwareIdVersion > 0x001C00) {
                        dspCommandOk = sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxSpeakerOutputEnable,
                                                          cirrus::devices::cs35l41::registers::kStatusMailboxRunning);
                        sequenceOk = sequenceOk && dspCommandOk;
                    } else {
                        checkedWrite(0x0000742C, 0x000000F9);
                        checkedWrite(0x00007438, 0x00580941);
                    }

                }

                bool locked = lockTestKey(amp);
                if (!sequenceOk || !locked) {
                    abortStart();
                    continue;
                }

                uint32_t readyPwr1 = 0, readyPwr2 = 0, readyDac = 0, readyAsp = 0;
                bool ready = readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl1, &readyPwr1) &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kPowerControl2, &readyPwr2) &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, &readyDac) &&
                             readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &readyAsp) && (readyPwr1 & 1) &&
                             (readyPwr2 & (0x3001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) == (dspMode ? 0x3001U : 1U) &&
                             readyDac == (dspMode ? 0x32U : 8U) && readyAsp == 0x10000U;
                bool streamStillActive = synchronizeHdaStream();
                hdaStreamActive = streamStillActive;
                if (!ready) {
                    abortStart();
                    continue;
                }
                if (!streamStillActive || !mHdaState.observed || mHdaState.lastStreamTag != startStreamTag ||
                    mHdaState.lastFormat != startStreamFormat) {
                    stopPlayback(amp);
                    continue;
                }
                if (!checkProtectionStatus(amp)) {
                    abortStart();
                    continue;
                }
                uint32_t pllSts = 0;
                int pllTimeout = 20;
                bool pllLocked = false;
                while (pllTimeout > 0) {
                    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, &pllSts, TRACE_PLAYBACK)) {
                        sequenceOk = false;
                        break;
                    }
                    if (pllSts & 2) {
                        pllLocked = true;
                        break;
                    }
                    IODelay(1000);
                    pllTimeout--;
                }
                if (!sequenceOk || !pllLocked) {
                    if (sequenceOk)
                        recordDiagnosticFailure(amp, DIAG_PLL_UNLOCKED, cirrus::devices::cs35l41::registers::kRegIrq1RawStatus3, 2, pllSts);
                    else
                        recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT);
                    amp.playbackFaulted = true;
                    amp.pllRetryCooldown = 4;
                    stopPlayback(amp);
                    continue;
                }

                uint32_t vspkPad = 0, vspkGpio = 0;
                if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegGpioPadControl, &vspkPad) ||
                    !readRegister(amp, cirrus::devices::cs35l41::registers::kRegGpio1Control1, &vspkGpio) ||
                    (vspkPad & 0x00030000) != 0x00010000 || (vspkGpio & 0x80009000) != 0x00008000) {
                    abortStart();
                    continue;
                }
                setProperty("ThinkBook_VSPK_Pad_Verified", true);
                checkedWrite(cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0x00000233);
                checkedWrite(cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x00008000);

                uint32_t postPwr1 = 0, postPwr2 = 0, postVol = 0, postGain = 0;
                bool stateReadable = readRegister(amp, 0x00002014, &postPwr1) && readRegister(amp, 0x00002018, &postPwr2) &&
                                     readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &postVol) &&
                                     readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &postGain);
                bool startVerified =
                    stateReadable && pupDone && dspCommandOk && sequenceOk && locked && postVol == 0x00008000 && (pllSts & 0x02) &&
                    ((postPwr1 & 0x1) != 0) && ((postPwr2 & 0x1) != 0) &&
                    ((postPwr2 & (0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) == (dspMode ? 0x00003000U : 0U)) &&
                    (postGain == 0x233);
                if (startVerified) {
                    amp.playbackActive = true;
                    markDiagnosticSuccess(amp, STAGE_PLAYBACK_ACTIVE);
                    char playbackProperty[80];
                    snprintf(playbackProperty, sizeof(playbackProperty), "Cirrus_Playback_Verdict_%s", amp.name);
                    OSString* playbackVerdict = OSString::withCString("ACTIVE_REGISTERS_VERIFIED_ROUTE_UNCONFIRMED");
                    if (playbackVerdict) {
                        setProperty(playbackProperty, playbackVerdict);
                        playbackVerdict->release();
                    }
                    CIRRUS_LOG("Background Monitor: Playback ENABLED and verified on %s: pwr1=0x%08X pwr2=0x%08X vol=0x%08X gain=0x%08X",
                               amp.name, postPwr1, postPwr2, postVol, postGain);
                    CIRRUS_LOG("DIAG_BOUNDARY amp=%s HDA_RUN=%d stream_tag=%u format=0x%04X PLL_LOCK=%d PUP_DONE=1 GLOBAL_EN=1 AMP_EN=1; "
                               "speaker converter route and physical audio remain unverified",
                               amp.name, streamStillActive, mHdaState.lastStreamTag, mHdaState.lastFormat, (pllSts & 2) != 0);
                    snapshotDiagnostics(amp, "PLAYBACK ACTIVE VERIFIED");
                } else {
                    CIRRUS_ERR("Background Monitor: Playback start verification failed on %s (read=%d pup=%d pwr1=0x%08X pwr2=0x%08X "
                               "gain=0x%08X); rolling back",
                               amp.name, stateReadable, pupDone, postPwr1, postPwr2, postGain);
                    recordDiagnosticFailure(amp, DIAG_PLAYBACK_INVARIANT, cirrus::devices::cs35l41::registers::kPowerControl1, 0x00000001,
                                            postPwr1);
                    abortStart();
                }
            }
        } else if (!hasAudio && amp.playbackActive) {
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 2) {
                stopPlayback(amp);
                amp.pllRetryCooldown = 0;
            }
        } else {
            amp.playbackStableCount = 0;
        }
    }

    if (mProbeTimer) {
        mProbeTimer->setTimeoutMS(mHdaState.streamActive ? 100 : 250);
    }
}

bool CirrusAudioFixup::transferToAddress(UInt8 address, UInt8* writeBuffer, UInt16 writeLength, UInt8* readBuffer, UInt16 readLength) {
    if (address > 0x7F || (writeLength && !writeBuffer) || (readLength && !readBuffer) || (!writeLength && !readLength)) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    if (!mProvider || !mPowerAvailable || mStopping) {
        mLastTransferReturn = kIOReturnNotReady;
        if (!mProbingAmplifiers)
            CIRRUS_ERR("transfer blocked: provider=%p powered=%d stopping=%d", mProvider, mPowerAvailable, mStopping);
        return false;
    }

    // Reuse the verified native controller API; no custom VoodooI2C replacement.
    if(address!=0x40&&address!=0x41)return false;
    if (writeLength > 4 && (!mOwnCalibrationValidated || !mManualArmed)) {
        mLastTransferReturn = kIOReturnNotPermitted;
        setProperty("ThinkBook_BlockedUncalibratedWrite", true);
        return false;
    }

    // Last boundary before the controller, including bulk/firmware transfers.
    if (readLength == 0 &&
        (writeLength < 5 || !thinkbookOutputWriteAllowed(readBE32(writeBuffer), writeBuffer + 4, writeLength - 4))) {
        mLastTransferReturn = kIOReturnNotPermitted;
        setProperty("ThinkBook_BlockedUnsafeWrite", true);
        return false;
    }
    VoodooI2CAddressedTransfer request{address,writeBuffer,writeLength,readBuffer,readLength};
    IOReturn ret=mProvider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS,true,&request,nullptr,nullptr,nullptr);
    mLastTransferReturn = ret;
    setProperty("CirrusTransferRet", (uint64_t)ret, 32);
    if (ret != kIOReturnSuccess) {
        if (!mProbingAmplifiers)
            CIRRUS_ERR("transfer address=0x%02X write=%u read=%u ret=0x%08X", address, writeLength, readLength, ret);
        return false;
    }

    return true;
}

void CirrusAudioFixup::recordTransfer(TraceSource, uint8_t, bool write, bool bulk, uint32_t, uint32_t, IOReturn ret) {
    if(bulk) {if(ret==kIOReturnSuccess)++mTraceStats.bulkSuccess;else ++mTraceStats.bulkFail;}
    else if(write) {if(ret==kIOReturnSuccess)++mTraceStats.writeSuccess;else ++mTraceStats.writeFail;}
    else {if(ret==kIOReturnSuccess)++mTraceStats.readSuccess;else ++mTraceStats.readFail;}
    if(ret==kIOReturnOffline)++mTraceStats.noackCount;
}

void CirrusAudioFixup::publishStatistics() {
    setProperty("AudioReads",mTraceStats.readSuccess,32);
    setProperty("AudioWrites",mTraceStats.writeSuccess,32);
    setProperty("AudioTransferErrors",mTraceStats.readFail+mTraceStats.writeFail+mTraceStats.bulkFail,32);
}

const char* CirrusAudioFixup::stageName(DriverStage stage) {
    switch (stage) {
    case STAGE_PROBE:
        return "PROBE";
    case STAGE_RESET:
        return "RESET";
    case STAGE_OTP_BOOT:
        return "OTP_BOOT";
    case STAGE_ERRATA:
        return "ERRATA";
    case STAGE_CLOCK:
        return "CLOCK";
    case STAGE_ASP:
        return "ASP";
    case STAGE_GPIO:
        return "GPIO";
    case STAGE_PLATFORM:
        return "PLATFORM";
    case STAGE_FIRMWARE_DISCOVERY:
        return "FIRMWARE_DISCOVERY";
    case STAGE_FIRMWARE_UPLOAD:
        return "FIRMWARE_UPLOAD";
    case STAGE_DSP_BOOT:
        return "DSP_BOOT";
    case STAGE_IDLE_VERIFY:
        return "IDLE_VERIFY";
    case STAGE_HDA_DETECT:
        return "HDA_DETECT";
    case STAGE_PLAYBACK_OPEN:
        return "PLAYBACK_OPEN";
    case STAGE_PLAYBACK_PREPARE:
        return "PLAYBACK_PREPARE";
    case STAGE_PLAYBACK_ACTIVE:
        return "PLAYBACK_ACTIVE";
    case STAGE_PLAYBACK_CLEANUP:
        return "PLAYBACK_CLEANUP";
    case STAGE_PLAYBACK_CLOSE:
        return "PLAYBACK_CLOSE";
    case STAGE_SAFE_IDLE:
        return "SAFE_IDLE";
    default:
        return "NONE";
    }
}

const char* CirrusAudioFixup::failureName(DiagnosticFailure failure) {
    switch (failure) {
    case DIAG_OK:
        return "OK";
    case DIAG_PROVIDER_MISSING:
        return "PROVIDER_MISSING";
    case DIAG_I2C_TRANSFER:
        return "I2C_TRANSFER";
    case DIAG_RESET_GPIO:
        return "RESET_GPIO";
    case DIAG_DEVICE_ID:
        return "DEVICE_ID";
    case DIAG_RESET_WRITE:
        return "RESET_WRITE";
    case DIAG_OTP_TIMEOUT:
        return "OTP_TIMEOUT";
    case DIAG_OTP_BOOT_ERROR:
        return "OTP_BOOT_ERROR";
    case DIAG_AMP_PROTECTION:
        return "AMP_PROTECTION";
    case DIAG_ERRATA:
        return "ERRATA";
    case DIAG_OTP_UNPACK:
        return "OTP_UNPACK";
    case DIAG_PLL_CONFIG:
        return "PLL_CONFIG";
    case DIAG_ASP_CONFIG:
        return "ASP_CONFIG";
    case DIAG_GPIO_CONFIG:
        return "GPIO_CONFIG";
    case DIAG_PLATFORM_CONFIG:
        return "PLATFORM_CONFIG";
    case DIAG_FIRMWARE_MISSING:
        return "FIRMWARE_MISSING";
    case DIAG_FIRMWARE_PARSE:
        return "FIRMWARE_PARSE";
    case DIAG_FIRMWARE_UPLOAD:
        return "FIRMWARE_UPLOAD";
    case DIAG_COEFFICIENT_PARSE:
        return "COEFFICIENT_PARSE";
    case DIAG_COEFFICIENT_UPLOAD:
        return "COEFFICIENT_UPLOAD";
    case DIAG_DSP_BOOT:
        return "DSP_BOOT";
    case DIAG_DSP_MAILBOX:
        return "DSP_MAILBOX";
    case DIAG_IDLE_INVARIANT:
        return "IDLE_INVARIANT";
    case DIAG_HDA_CONTROLLER:
        return "HDA_CONTROLLER";
    case DIAG_HDA_STREAM_FORMAT:
        return "HDA_STREAM_FORMAT";
    case DIAG_PLL_UNLOCKED:
        return "PLL_UNLOCKED";
    case DIAG_POWER_UP_TIMEOUT:
        return "POWER_UP_TIMEOUT";
    case DIAG_PLAYBACK_INVARIANT:
        return "PLAYBACK_INVARIANT";
    case DIAG_POWER_DOWN_TIMEOUT:
        return "POWER_DOWN_TIMEOUT";
    case DIAG_IDLE_ROLLBACK:
        return "IDLE_ROLLBACK";
    case DIAG_CALIBRATION:
        return "CALIBRATION";
    default:
        return "UNKNOWN";
    }
}

void CirrusAudioFixup::setDiagnosticStage(AmplifierState& amp, DriverStage stage) {
    DriverStage previous = amp.diagnostic.stage;
    amp.diagnostic.stage = stage;
    if (previous != stage) {
        CIRRUS_LOG("DIAG_STAGE amp=%s from=%s to=%s", amp.name, stageName(previous), stageName(stage));
    }
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Diag_Stage_%s", amp.name);
    OSString* value = OSString::withCString(stageName(stage));
    if (value) {
        setProperty(property, value);
        value->release();
    }
}

void CirrusAudioFixup::markDiagnosticSuccess(AmplifierState& amp, DriverStage stage) {
    amp.diagnostic.stage = stage;
    amp.diagnostic.lastGoodStage = stage;
    char property[80];
    snprintf(property, sizeof(property), "Cirrus_Diag_LastGood_%s", amp.name);
    OSString* value = OSString::withCString(stageName(stage));
    if (value) {
        setProperty(property, value);
        value->release();
    }
    CIRRUS_LOG("DIAG_PASS amp=%s stage=%s", amp.name, stageName(stage));
    setDiagnosticStage(amp, stage);
}

void CirrusAudioFixup::recordDiagnosticFailure(AmplifierState& amp, DiagnosticFailure failure, UInt32 reg, UInt32 expected, UInt32 actual,
                                               IOReturn ioReturn, bool) {
    DiagnosticState& diag = amp.diagnostic;
    bool first = diag.firstFailure == DIAG_OK;
    bool changed =
        diag.latestFailure != failure || diag.reg != reg || diag.actual != actual || diag.expected != expected || diag.ioReturn != ioReturn;
    if (first)
        diag.firstFailure = failure;
    diag.latestFailure = failure;
    diag.failureCount++;
    diag.reg = reg;
    diag.expected = expected;
    diag.actual = actual;
    diag.ioReturn = ioReturn;

    bool publishNow = first || changed || ((diag.failureCount % 32U) == 0);
    if (publishNow) {
        char property[96];
        OSString* value = nullptr;
        snprintf(property, sizeof(property), "Cirrus_Diag_FirstFailure_%s", amp.name);
        value = OSString::withCString(failureName(diag.firstFailure));
        if (value) {
            setProperty(property, value);
            value->release();
        }
        snprintf(property, sizeof(property), "Cirrus_Diag_LatestFailure_%s", amp.name);
        value = OSString::withCString(failureName(failure));
        if (value) {
            setProperty(property, value);
            value->release();
        }
        snprintf(property, sizeof(property), "Cirrus_Diag_FailureCount_%s", amp.name);
        setProperty(property, diag.failureCount, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_FailureReg_%s", amp.name);
        setProperty(property, reg, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_Expected_%s", amp.name);
        setProperty(property, expected, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_Actual_%s", amp.name);
        setProperty(property, actual, 32);
        snprintf(property, sizeof(property), "Cirrus_Diag_IOReturn_%s", amp.name);
        setProperty(property, (uint64_t)ioReturn, 32);
        publishStatistics();
    }

    if (first || changed) {
        CIRRUS_ERR("DIAG_FAIL amp=%s stage=%s code=%s count=%u reg=0x%08X expected=0x%08X actual=0x%08X io=0x%08X first=%s", amp.name,
                   stageName(diag.stage), failureName(failure), diag.failureCount, reg, expected, actual, ioReturn,
                   failureName(diag.firstFailure));
    }

}

void CirrusAudioFixup::publishDriverVerdict() {
    bool allInitialized = (mAmpCount > 0);
    bool allDsp = (mAmpCount > 0);
    bool anyFaulted = false;
    for (size_t i = 0; i < mAmpCount; ++i) {
        if (!mAmps[i].present)
            continue;
        if (!mAmps[i].initialized)
            allInitialized = false;
        if (!mAmps[i].dspAlive || !mAmps[i].firmwareValidated)
            allDsp = false;
        if (mAmps[i].playbackFaulted)
            anyFaulted = true;
    }
    const char* verdict = !mPowerAvailable                 ? "SUSPENDED"
                          
                          : anyFaulted                     ? "FAULT_LATCHED"
                          : !allInitialized                ? "FAILED_INIT"
                          : allDsp                         ? "READY_DSP"
                          
                                                           : "OUTPUT_DISABLED_DSP_UNVERIFIED";
    OSString* value = OSString::withCString(verdict);
    if (value) {
        setProperty("Cirrus_Driver_Verdict", value);
        value->release();
    }
    CIRRUS_LOG("DIAG_VERDICT driver=%s amps=%lu left[first=%s latest=%s last_good=%s] right[first=%s latest=%s last_good=%s]", verdict,
               mAmpCount, failureName(mAmps[0].diagnostic.firstFailure), failureName(mAmps[0].diagnostic.latestFailure),
               stageName(mAmps[0].diagnostic.lastGoodStage), failureName(mAmps[1].diagnostic.firstFailure),
               failureName(mAmps[1].diagnostic.latestFailure), stageName(mAmps[1].diagnostic.lastGoodStage));
}

bool CirrusAudioFixup::bulkRead(AmplifierState& amp, UInt32 reg, UInt8* data, size_t length, TraceSource source) {
    if (!data || length == 0 || length > 0xFFFFU) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 0xFFFFU, (UInt32)length, kIOReturnBadArgument, false);
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    bool success = transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), data, (UInt16)length);

    IOReturn retCode = mLastTransferReturn;
    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;
    recordTransfer(source, ampIdx, false, true, reg, (UInt32)length, retCode);
    if (!success && !mCapturingFailureSnapshot) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, (UInt32)length, 0, mLastTransferReturn);
    }
    mLastTransferReturn = retCode;
    return success;
}

bool CirrusAudioFixup::bulkWrite(AmplifierState& amp, UInt32 reg, const UInt8* data, size_t length, TraceSource source) {
    if ((length > 0 && !data) || length > (0xFFFFU - 4U)) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 0xFFFFU - 4U, (UInt32)length, kIOReturnBadArgument, false);
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    UInt8 stackBuffer[8];
    UInt8* writeBuffer = stackBuffer;
    bool useMalloc = (4 + length > sizeof(stackBuffer));

    if (useMalloc) {
        writeBuffer = (UInt8*)IOMallocData(4 + length);
        if (!writeBuffer) {
            recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, (UInt32)length, 0, kIOReturnNoMemory, false);
            mLastTransferReturn = kIOReturnNoMemory;
            return false;
        }
    }

    writeBE32(writeBuffer, reg);
    if (length > 0 && data) {
        memcpy(writeBuffer + 4, data, length);
    }

    bool ret = transferToAddress(amp.address, writeBuffer, static_cast<UInt16>(4 + length), nullptr, 0);

    IOReturn retCode = mLastTransferReturn;
    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;
    recordTransfer(source, ampIdx, true, true, reg, (uint32_t)length, retCode);
    if (!ret && !mCapturingFailureSnapshot) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, (UInt32)length, 0, mLastTransferReturn);
    }

    if (useMalloc) {
        IOFreeData(writeBuffer, 4 + length);
    }
    mLastTransferReturn = retCode;
    return ret;
}

bool CirrusAudioFixup::readRegister(AmplifierState& amp, UInt32 reg, UInt32* value, TraceSource source) {
    if (!value) {
        recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 4, 0, kIOReturnBadArgument, false);
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    UInt8 readBuffer[4]{0};

    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    bool success = transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), readBuffer, sizeof(readBuffer));

    IOReturn retCode = mLastTransferReturn;
    uint8_t ampIdx = (amp.address == cirrus::devices::cs35l41::registers::kI2cAddressRight) ? 1 : 0;

    if (success) {
        *value = readBE32(readBuffer);
        recordTransfer(source, ampIdx, false, false, reg, *value, retCode);
        return true;
    } else {
        recordTransfer(source, ampIdx, false, false, reg, 0, retCode);
        if (!mCapturingFailureSnapshot) {
            recordDiagnosticFailure(amp, DIAG_I2C_TRANSFER, reg, 4, 0, mLastTransferReturn);
        }
        mLastTransferReturn = retCode;
        return false;
    }
}

bool CirrusAudioFixup::writeRegister(AmplifierState& amp, UInt32 reg, UInt32 value, TraceSource source) {
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, value);
    bool success = bulkWrite(amp, reg, writeBuffer, sizeof(writeBuffer), source);

    return success;
}

bool CirrusAudioFixup::updateRegisterBits(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 value, TraceSource source) {
    UInt32 currentVal = 0;
    if (!readRegister(amp, reg, &currentVal, source)) {
        CIRRUS_LOG("updateRegisterBits failed: read error at 0x%05X", reg);
        return false;
    }

    UInt32 newVal = (currentVal & ~mask) | (value & mask);

    if (newVal == currentVal) {
        return true;
    }

    return writeRegister(amp, reg, newVal, source);
}

bool CirrusAudioFixup::pollRegisterBit(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs,
                                       TraceSource source) {
    UInt32 val = 0;
    for (UInt32 i = 0; i < timeoutMs; i++) {
        if (!readRegister(amp, reg, &val, source)) {
            return false;
        }
        if ((val & mask) == targetVal) {
            CIRRUS_LOG("Amp %s: poll reg=0x%05X mask=0x%X expect=0x%X elapsed=%dms iterations=%d", amp.name, reg, mask, targetVal, i + 1,
                       i + 1);
            return true;
        }
        IODelay(1000);
    }

    CIRRUS_ERR("Amp %s: pollRegisterBit timeout! reg=0x%05X, mask=0x%X, val=0x%08X", amp.name, reg, mask, val);

    UInt32 st1 = 0, st2 = 0, st3 = 0, st4 = 0, pwr_ctrl2 = 0;
    readRegister(amp, 0x10010, &st1, source);
    readRegister(amp, 0x10014, &st2, source);
    readRegister(amp, 0x10018, &st3, source);
    readRegister(amp, 0x1001C, &st4, source);
    readRegister(amp, 0x02018, &pwr_ctrl2, source);
    CIRRUS_ERR("Amp %s: DIAGNOSTICS -> ST1=0x%08X ST2=0x%08X ST3=0x%08X ST4=0x%08X PWR_CTRL2=0x%08X", amp.name, st1, st2, st3, st4,
               pwr_ctrl2);

    return false;
}

bool CirrusAudioFixup::sendMailboxCommand(AmplifierState& amp, UInt32 command, UInt32 expectedStatus) {
    if (!writeRegister(amp, 0x00013020, command, TRACE_PLAYBACK)) {
        recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, 0x00013020, command, 0, mLastTransferReturn);
        return false;
    }

    UInt32 status = 0;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        IODelay(1000);
        if (!readRegister(amp, cirrus::devices::cs35l41::registers::kDspMbox2Register, &status, TRACE_PLAYBACK))
            return false;
        if (status == 0xFFFFFFFFU || status == 0x00FFFFFFU) {
            CIRRUS_ERR("DSP mailbox reported error on %s for command %u: 0x%08X", amp.name, command, status);
            recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::cs35l41::registers::kDspMbox2Register, expectedStatus, status);
            return false;
        }
        if (status == expectedStatus)
            return true;
    }

    CIRRUS_ERR("DSP mailbox command %u timed out on %s (expected=%u status=0x%08X)", command, amp.name, expectedStatus, status);
    recordDiagnosticFailure(amp, DIAG_DSP_MAILBOX, cirrus::devices::cs35l41::registers::kDspMbox2Register, expectedStatus, status);
    return false;
}

bool CirrusAudioFixup::initCodec(AmplifierState& amp) {
    setDiagnosticStage(amp,STAGE_PROBE);
    amp.present=false;
    FixupRegisterIOAdapter io(this,amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name,amp.address);
    if(!device.probe(io))return false;
    amp.deviceId=device.deviceId();amp.revisionId=device.revisionId();amp.present=device.isPresent();
    if(!amp.present || amp.deviceId!=0x35A40 || (amp.revisionId&0xFF)!=0xB2)return false;
    setDiagnosticStage(amp,STAGE_RESET);
    auto result=device.initialize(io);
    if(result!=cirrus::core::AudioDevice::InitResult::Success)return false;
    setDiagnosticStage(amp,STAGE_OTP_BOOT);
    uint32_t id=0,revision=0;
    if(!readRegister(amp,0,&id)||!readRegister(amp,4,&revision))return false;
    amp.deviceId=id;amp.revisionId=revision;
    amp.present=id==0x35A40 && (revision&0xFF)==0xB2;
    return amp.present;
}

bool CirrusAudioFixup::unlockTestKey(AmplifierState& amp) {
    bool ret1 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x00000055);
    bool ret2 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x000000AA);

    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to unlock test keys on %s", amp.name);
        return false;
    }

    CIRRUS_LOG("test keys unlocked successfully on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::lockTestKey(AmplifierState& amp) {
    bool ret1 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x000000CC);
    bool ret2 = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegTestKeyControl, 0x00000033);

    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to lock test keys on %s", amp.name);
        return false;
    }

    CIRRUS_LOG("test keys locked successfully on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::initializeHardwareErrata(AmplifierState& amp) {
    if(!unlockTestKey(amp))return false;
    const bool ok=applyErrataPatch(amp) && unpackOTP(amp);
    const bool locked=lockTestKey(amp);
    if(!locked)amp.playbackFaulted=true;
    return ok && locked;
}

bool CirrusAudioFixup::applyErrataPatch(AmplifierState& amp) {
    const ErrataTable errataTables[] = {{0xB2, cs35l41_revb2_errata_patch, sizeof(cs35l41_revb2_errata_patch) / sizeof(ErrataPatch)}};

    UInt32 revOnly = amp.revisionId & 0xFF;
    const ErrataTable* tableToApply = nullptr;

    for (size_t i = 0; i < sizeof(errataTables) / sizeof(ErrataTable); i++) {
        if (errataTables[i].revid == revOnly) {
            tableToApply = &errataTables[i];
            break;
        }
    }

    if (!tableToApply) {
        CIRRUS_ERR("no errata patches found for revision 0x%02X on %s", revOnly, amp.name);
        return false;
    }

    CIRRUS_LOG("applying %lu errata patches for revision 0x%02X on %s", tableToApply->numPatches, revOnly, amp.name);

    for (size_t i = 0; i < tableToApply->numPatches; i++) {
        if (!writeRegister(amp, tableToApply->patches[i].reg, tableToApply->patches[i].value)) {
            CIRRUS_ERR("failed to apply errata patch at register 0x%08X on %s", tableToApply->patches[i].reg, amp.name);
            recordDiagnosticFailure(amp, DIAG_ERRATA, tableToApply->patches[i].reg, tableToApply->patches[i].value, 0, mLastTransferReturn);
            return false;
        }
    }

    if (!updateRegisterBits(amp, 0x02BC1000, cirrus::devices::cs35l41::registers::kValHaloCoreEnable, 0x00000000)) {
        CIRRUS_ERR("failed to disable dsp core in core ctrl on %s", amp.name);
        return false;
    }

    return true;
}

bool CirrusAudioFixup::unpackOTP(AmplifierState& amp) {
    FixupRegisterIOAdapter io(this, amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name, amp.address);
    if (!device.unpackOTP(io)) {
        CIRRUS_ERR("failed to unpack OTP on %s", amp.name);
        return false;
    }
    CIRRUS_LOG("otp unpacking complete on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sequence[i].updateBits) {
            if (!updateRegisterBits(amp, sequence[i].reg, sequence[i].mask, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        } else {
            if (!writeRegister(amp, sequence[i].reg, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        }
        if (sequence[i].delayUs > 0) {
            IODelay(sequence[i].delayUs);
        }
    }
    return true;
}

bool CirrusAudioFixup::applyPLL(AmplifierState& amp) {
    FixupRegisterIOAdapter io(this,amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name,amp.address);
    bool locked=false;return device.setupPLL(io,&locked);
}

bool CirrusAudioFixup::applyASP(AmplifierState& amp) {
    FixupRegisterIOAdapter io(this,amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name,amp.address);
    return device.setupASP(io,amp.address==0x41,amp.firmwareValidated);
}

bool CirrusAudioFixup::applyGPIO(AmplifierState& amp) {
    FixupRegisterIOAdapter io(this,amp);
    cirrus::devices::cs35l41::CS35L41Device device(amp.name,amp.address);
    return device.setupGPIO(io);
}

IOService* CirrusAudioFixup::audioController() {
    auto* matching=serviceMatching("AppleHDAController");
    if(!matching)return nullptr;
    auto* iterator=getMatchingServices(matching);matching->release();
    if(!iterator)return nullptr;
    IOPCIDevice* result=nullptr;
    while(auto* entry=OSDynamicCast(IORegistryEntry,iterator->getNextObject())) {
        for(unsigned depth=0;entry&&depth<8;++depth,entry=entry->getParentEntry(gIOServicePlane)) {
            auto* pci=OSDynamicCast(IOPCIDevice,entry);
            if(!pci)continue;
            if(pci->configRead16(kIOPCIConfigVendorID)==0x8086 && pci->configRead16(kIOPCIConfigDeviceID)==0x51CA &&
               pci->getName() && strcmp(pci->getName(),"HDEF")==0) {result=pci;result->retain();}
            break;
        }
        if(result)break;
    }
    iterator->release();return result;
}
IOService* CirrusAudioFixup::getAudioController() {
    if(!mHdaControllerCache)mHdaControllerCache=audioController();
    if(mHdaControllerCache)mHdaControllerCache->retain();
    return mHdaControllerCache;
}

bool CirrusAudioFixup::stopDSP(AmplifierState& amp) {
    amp.dspAlive = false;
    amp.monitorCount = 0;
    bool halted = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl,
                                     cirrus::devices::cs35l41::registers::kValHaloCoreEnable, 0);
    bool reset = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegDsp1CoreSoftReset, 1, 1);
    IODelay(1000);
    uint32_t core = 0;
    return halted && reset && readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &core) &&
           !(core & cirrus::devices::cs35l41::registers::kValHaloCoreEnable);
}

void CirrusAudioFixup::discoverFirmware(AmplifierState& amp) {
    amp.firmwareValidated=false;
    amp.wmfwData=amp.binData=nullptr;amp.wmfwSize=amp.binSize=0;
    if(cs35l41FirmwareCount!=1 || (amp.address!=0x40 && amp.address!=0x41))return;
    const auto& resource=cs35l41Firmware[0];
    if(resource.subsystemVendor!=0x17AA || resource.subsystemDevice!=0x38A9 || resource.spkid!=0 || resource.isDummy)return;
    const uint8_t* tuning=amp.address==0x41?resource.binRight:resource.bin;
    const size_t length=amp.address==0x41?resource.binRightSize:resource.binSize;
    if(!resource.wmfw || resource.wmfwSize<12 || !tuning || !length || resource.wmfwSize%4 || length%4)return;
    if(memcmp(resource.wmfw,"WMFW",4)!=0)return;
    amp.wmfwData=resource.wmfw;amp.wmfwSize=resource.wmfwSize;amp.binData=tuning;amp.binSize=length;
}

bool CirrusAudioFixup::bringupDSP(AmplifierState& amp) {
    if (!amp.firmwareValidated || !amp.haloStateRegister || !amp.haloHeartbeatRegister)
        return false;
    auto failBoot = [&]() -> bool {
        recordDiagnosticFailure(amp, DIAG_DSP_BOOT);
        if (!stopDSP(amp))
            amp.playbackFaulted = true;
        return false;
    };
    bool writesOk = true;
    auto checkedWrite = [&](uint32_t reg, uint32_t value) {
        if (writesOk)
            writesOk = writeRegister(amp, reg, value, TRACE_FIRMWARE);
    };
    char propName[64];
    OSString* statusStr = nullptr;

    uint32_t preCoreCtrl = 0, preClkCtrl = 0, preMbox = 0;
    uint32_t preSysId = 0, preSysVer = 0, preSysCore = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &preCoreCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspClockControl, &preClkCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &preMbox))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &preSysId))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemVersion, &preSysVer))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemCoreId, &preSysCore))
        return failBoot();
    CIRRUS_LOG("dsp pre-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X", amp.name,
               preCoreCtrl, preClkCtrl, preMbox, preSysId, preSysVer, preSysCore);

    static const uint32_t fsRateRegs[] = {
        cirrus::devices::cs35l41::registers::kRegDsp1Rx1Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx2Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Rx3Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx4Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Rx5Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx6Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Rx7Rate, cirrus::devices::cs35l41::registers::kRegDsp1Rx8Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx1Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx2Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx3Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx4Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx5Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx6Rate,
        cirrus::devices::cs35l41::registers::kRegDsp1Tx7Rate, cirrus::devices::cs35l41::registers::kRegDsp1Tx8Rate};
    bool fsErrataOk = true;
    for (size_t i = 0; i < sizeof(fsRateRegs) / sizeof(fsRateRegs[0]); ++i) {
        fsErrataOk &= writeRegister(amp, fsRateRegs[i], 0x00000001, TRACE_FIRMWARE);
    }
    CIRRUS_LOG("DSP FS errata on %s: %s", amp.name, fsErrataOk ? "applied" : "FAILED");
    if (!fsErrataOk)
        return failBoot();

    uint64_t mpuStart = mach_absolute_time();

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuLockConfig, 0x5555);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuLockConfig, 0xAAAA);

    uint32_t unlockVal = 0xFFFFFFFF;
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess0, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess0, unlockVal);

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess1, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess1, unlockVal);

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess2, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess2, unlockVal);

    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXmAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYmAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuWndwAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuXregAccess3, unlockVal);
    checkedWrite(cirrus::devices::cs35l41::registers::kRegDsp1MpuYregAccess3, unlockVal);

    bool mpuLocked = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1MpuLockConfig, 0);
    if (!writesOk || !mpuLocked)
        return failBoot();

    uint64_t mpuEnd = mach_absolute_time();
    (void)mpuStart;
    (void)mpuEnd;

    snprintf(propName, sizeof(propName), "Cirrus_DSP_MEM_WINDOW_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    uint32_t currentClk = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspClockControl, &currentClk))
        return failBoot();
    snprintf(propName, sizeof(propName), "Cirrus_DSP_CLOCK_RAW_%s", amp.name);
    setProperty(propName, (uint64_t)currentClk, 32);

    uint64_t resetStart = mach_absolute_time();

    if (!updateRegisterBits(
            amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl,
            cirrus::devices::cs35l41::registers::kValHaloCoreReset | cirrus::devices::cs35l41::registers::kValHaloCoreEnable,
            cirrus::devices::cs35l41::registers::kValHaloCoreReset | cirrus::devices::cs35l41::registers::kValHaloCoreEnable) ||
        !updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl,
                            cirrus::devices::cs35l41::registers::kValHaloCoreReset, 0))
        return failBoot();

    uint64_t resetEnd = mach_absolute_time();
    uint64_t resetTimeUs = (resetEnd - resetStart) / 1000;

    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_TIME_US_%s", amp.name);
    setProperty(propName, resetTimeUs, 32);

    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    uint32_t sysId = 0, sysVer = 0, sysCore = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sysId))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemVersion, &sysVer))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemCoreId, &sysCore))
        return failBoot();

    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_ID_%s", amp.name);
    setProperty(propName, (uint64_t)sysId, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_VER_%s", amp.name);
    setProperty(propName, (uint64_t)sysVer, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_CORE_%s", amp.name);
    setProperty(propName, (uint64_t)sysCore, 32);

    uint32_t postCoreCtrl = 0, postClkCtrl = 0, postMbox = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &postCoreCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspClockControl, &postClkCtrl))
        return failBoot();
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &postMbox))
        return failBoot();
    CIRRUS_LOG("dsp post-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X",
               amp.name, postCoreCtrl, postClkCtrl, postMbox, sysId, sysVer, sysCore);

    snprintf(propName, sizeof(propName), "Cirrus_DSP_STATE_%s", amp.name);
    if (sysId == 0xFFFFFFFF && sysVer == 0xFFFFFFFF && sysCore == 0xFFFFFFFF) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_UNREACHABLE)");
        if (statusStr) {
            setProperty(propName, statusStr);
            statusStr->release();
        }
        return failBoot();
    } else if (sysId == 0x00000000 && sysVer == 0x00000000 && sysCore == 0x00000000) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_IN_RESET)");
        if (statusStr) {
            setProperty(propName, statusStr);
            statusStr->release();
        }
        return failBoot();
    }

    uint32_t mboxInit = 0;
    if (!readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &mboxInit))
        return failBoot();
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_RAW_INIT_%s", amp.name);
    setProperty(propName, (uint64_t)mboxInit, 32);

    const uint32_t HALO_STATE_REG = amp.haloStateRegister;
    const uint32_t HALO_STATE_CODE_RUN = 2;
    uint32_t haloState = 0;
    int haloTimeout = 15;
    while (haloTimeout > 0) {
        if (!readRegister(amp, HALO_STATE_REG, &haloState))
            return failBoot();
        if (haloState == HALO_STATE_CODE_RUN)
            break;
        IODelay(1000);
        haloTimeout--;
    }
    if (haloState == HALO_STATE_CODE_RUN) {
        CIRRUS_LOG("HALO firmware reached RUN state on %s (halo_state=0x%08X)", amp.name, haloState);

        if (!sendMailboxCommand(amp, cirrus::devices::cs35l41::registers::kCmdMailboxPause,
                                cirrus::devices::cs35l41::registers::kStatusMailboxPaused))
            return failBoot();
    } else {
        CIRRUS_ERR("HALO firmware did NOT reach RUN on %s (halo_state=0x%08X, expected 0x2). "
                   "DSP boot likely failed — check wmfw/coeff upload PASS count above.",
                   amp.name, haloState);
    }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HALO_STATE_%s", amp.name);
    setProperty(propName, (uint64_t)haloState, 32);

    const uint32_t HALO_HEARTBEAT_REG = amp.haloHeartbeatRegister;
    uint32_t hb0 = 0, hb1 = 0, hb2 = 0;
    bool hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb0);
    IODelay(2000);
    hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb1) && hbReadable;
    IODelay(2000);
    hbReadable = readRegister(amp, HALO_HEARTBEAT_REG, &hb2) && hbReadable;
    const bool heartbeatMoving = (hb0 != hb1) && (hb1 != hb2);
    const char* hbVerdict;
    if (!hbReadable) {
        hbVerdict = "READ_FAILED";
    } else if (heartbeatMoving) {
        hbVerdict = "CHANGING";
    } else {
        hbVerdict = "UNCHANGED_WHILE_PAUSED_NOT_A_BOOT_VERDICT";
    }
    CIRRUS_LOG("dsp heartbeat on %s: t0=0x%08X t1=0x%08X t2=0x%08X -> %s", amp.name, hb0, hb1, hb2, hbVerdict);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_%s", amp.name);
    setProperty(propName, (uint64_t)hb2, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_VERDICT_%s", amp.name);
    statusStr = OSString::withCString(hbVerdict);
    if (statusStr) {
        setProperty(propName, statusStr);
        statusStr->release();
    }

    if (haloState != HALO_STATE_CODE_RUN) {
        uint32_t xmVioSts = 0, xmVioAddr = 0;
        uint32_t ymVioSts = 0, ymVioAddr = 0;
        uint32_t pmVioSts = 0, pmVioAddr = 0;
        if (!readRegister(amp, 0x02BC3104, &xmVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3100, &xmVioAddr))
            return failBoot();
        if (!readRegister(amp, 0x02BC310C, &ymVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3108, &ymVioAddr))
            return failBoot();
        if (!readRegister(amp, 0x02BC3114, &pmVioSts))
            return failBoot();
        if (!readRegister(amp, 0x02BC3110, &pmVioAddr))
            return failBoot();

        bool actualMpuFault = ((xmVioSts & 0x007E0000) != 0) || ((ymVioSts & 0x007E0000) != 0) || ((pmVioSts & 0x007E0000) != 0);

        CIRRUS_ERR("MPU FAULT REPORT on %s: XM vio_sts=0x%08X vio_addr=0x%08X | "
                   "YM vio_sts=0x%08X vio_addr=0x%08X | PM vio_sts=0x%08X vio_addr=0x%08X",
                   amp.name, xmVioSts, xmVioAddr, ymVioSts, ymVioAddr, pmVioSts, pmVioAddr);
        if (actualMpuFault) {
            CIRRUS_ERR("  -> MPU BLOCKED an access. A non-zero vio_sts (bits 17-22) means the core was "
                       "denied memory access (MPU still locked / wrong region). This is the fault cause.");
        } else {
            CIRRUS_ERR("  -> No MPU violation latched. Core stall is NOT an MPU block; "
                       "suspect firmware image integrity (addressing/byte-order) or missing errata.");
        }

        uint32_t scratch[4] = {0};
        if (!readRegister(amp, 0x02B805C0, &scratch[0]))
            return failBoot();
        if (!readRegister(amp, 0x02B805C8, &scratch[1]))
            return failBoot();
        if (!readRegister(amp, 0x02B805D0, &scratch[2]))
            return failBoot();
        if (!readRegister(amp, 0x02B805D8, &scratch[3]))
            return failBoot();
        CIRRUS_ERR("DSP SCRATCH on %s: s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X "
                   "(non-zero => firmware ROM wrote a panic/abort code)",
                   amp.name, scratch[0], scratch[1], scratch[2], scratch[3]);

        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_XM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)xmVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_YM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)ymVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_PM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)pmVioSts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_SCRATCH1_%s", amp.name);
        setProperty(propName, (uint64_t)scratch[0], 32);
    }

    if (!hbReadable || haloState != HALO_STATE_CODE_RUN)
        return failBoot();
    CIRRUS_LOG("dsp bringup complete for %s", amp.name);
    return true;
}

bool CirrusAudioFixup::verifyDSPAlive(AmplifierState& amp) {
    uint32_t coreCtrl = 0, sysId = 0, mbox = 0, haloState = 0;
    bool corePass = false, resetPass = false, sysIdPass = false, mboxPass = false, xmPass = false;

    bool readable = amp.haloStateRegister && amp.haloHeartbeatRegister &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1CcmCoreControl, &coreCtrl, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1SystemId, &sysId, TRACE_DUMP) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDspMailbox2, &mbox, TRACE_DUMP) &&
                    readRegister(amp, amp.haloStateRegister, &haloState, TRACE_DUMP);

    if (coreCtrl & cirrus::devices::cs35l41::registers::kValHaloCoreEnable)
        corePass = true;
    if ((coreCtrl & cirrus::devices::cs35l41::registers::kValHaloCoreReset) == 0)
        resetPass = true;
    if (sysId != 0x00000000 && sysId != 0xFFFFFFFF)
        sysIdPass = true;

    if (mbox == cirrus::devices::cs35l41::registers::kStatusMailboxPaused)
        mboxPass = true;

    uint8_t dummy[4] = {0};
    if (bulkRead(amp, 0x02000000, dummy, 4, TRACE_DUMP)) {
        xmPass = true;
    }

    uint32_t xmVio = 0, xmVioAddr = 0;
    uint32_t ymVio = 0, ymVioAddr = 0;
    uint32_t pmVio = 0, pmVioAddr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    readable = readRegister(amp, 0x2BC3104, &xmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3100, &xmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC310C, &ymVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3108, &ymVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3114, &pmVio, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2BC3110, &pmVioAddr, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP) && readable;
    readable = readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP) && readable;

    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpuClean = ((xmVio & MPU_VIO_MASK) == 0 && (ymVio & MPU_VIO_MASK) == 0 && (pmVio & MPU_VIO_MASK) == 0);

    CIRRUS_LOG("dsp status verify results for %s: core_reset=%s, core_en=%s, sysid=%s(0x%08X), "
               "mailbox=%s(fw_status=%u), halo_state=0x%08X, xm_read=%s",
               amp.name, resetPass ? "ok" : "fail", corePass ? "ok" : "fail", sysIdPass ? "ok" : "fail", sysId, mboxPass ? "ok" : "fail",
               mbox, haloState, xmPass ? "ok" : "fail");
    CIRRUS_LOG("dsp verify MPU/scratch for %s: mpu=%s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X "
               "pm_vio=0x%08X@0x%08X scratch=[0x%08X 0x%08X 0x%08X 0x%08X]",
               amp.name, mpuClean ? "clean" : "FAULT", xmVio, xmVioAddr, ymVio, ymVioAddr, pmVio, pmVioAddr, sc1, sc2, sc3, sc4);

    uint32_t hb0 = 0, hb1 = 0;
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb0, TRACE_DUMP) && readable;
    IODelay(2000);
    readable = readRegister(amp, amp.haloHeartbeatRegister, &hb1, TRACE_DUMP) && readable;

    const uint32_t HALO_STATE_RUN = 2;
    bool runPass = (haloState == HALO_STATE_RUN);
    if (!runPass || !mpuClean || !mboxPass) {
        CIRRUS_ERR("dsp verdict for %s: halo=0x%08X heartbeat=0x%08X->0x%08X mpu=%s; DSP mode disabled", amp.name, haloState, hb0, hb1,
                   mpuClean ? "clean" : "FAULT");
    }

    return readable && corePass && resetPass && sysIdPass && mboxPass && xmPass && runPass && mpuClean;
}

bool CirrusAudioFixup::parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage) {
    uint8_t table[40 + 31 * 24] = {};
    outImage.algorithmCount = 0;
    if (!bulkRead(amp, 0x02800000, table, 40, TRACE_FIRMWARE))
        return false;
    uint32_t count = CirrusFirmwareParser::readUnpacked32BE(table, 9);
    if (!count || count > 31)
        return false;
    size_t length = 40 + count * 24;
    for (size_t offset = 40; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > 252)
            chunk = 252;
        if (!bulkRead(amp, static_cast<uint32_t>(0x02800000 + offset), table + offset, chunk, TRACE_FIRMWARE))
            return false;
        offset += chunk;
    }
    return CirrusFirmwareParser::parseAlgorithmTable(table, length, outImage);
}

bool CirrusAudioFixup::configureHardware(AmplifierState& amp) {
    CIRRUS_LOG("configuring generic serial port settings on %s", amp.name);
    bool ok = true;

    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask1, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask2, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask3, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq1Mask4, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask1, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask2, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask3, 0xFFFFFFFF) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegIrq2Mask4, 0xFFFFFFFF) && ok;

    ok = writeRegister(amp, 0x00002020, 0x00000006) && ok;

    if (ok)
        CIRRUS_LOG("generic serial port config complete on %s", amp.name);
    return ok;
}

void CirrusAudioFixup::snapshotDiagnostics(AmplifierState& amp, const char* stage) {
    auto* state=OSDictionary::withCapacity(5);
    if(!state)return;
    auto* name=OSString::withCString(stage);
    if(name){state->setObject("Stage",name);name->release();}
    state->setObject("Initialized",amp.initialized?kOSBooleanTrue:kOSBooleanFalse);
    state->setObject("DspHealthy",amp.dspAlive&&amp.firmwareValidated?kOSBooleanTrue:kOSBooleanFalse);
    state->setObject("PlaybackActive",amp.playbackActive?kOSBooleanTrue:kOSBooleanFalse);
    state->setObject("Faulted",amp.playbackFaulted?kOSBooleanTrue:kOSBooleanFalse);
    char property[64];snprintf(property,sizeof(property),"AudioState_%s",amp.name);
    setProperty(property,state);state->release();publishStatistics();
}

bool CirrusAudioFixup::powerUpAmplifier(AmplifierState& amp) {
    CIRRUS_LOG("starting power up sequence for %s (DSP Mode=%d)", amp.name, amp.monitorCount);
    bool ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678);
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0) && ok;
    if (!ok || !checkProtectionStatus(amp)) {
        amp.playbackFaulted = true;
        stopPlayback(amp);
        return false;
    }

    if (amp.monitorCount == 1) {
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010000) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000003) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x00000032) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000028) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000029) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000029) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx6Source, 0x00000029) && ok;
    } else {
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, 0x00010000) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortHighImpedanceControl, 0x00000002) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, 0x08) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx3Source, 0x00000032) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAspTx4Source, 0x00000033) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx1Source, 0x00000008) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx2Source, 0x00000009) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx3Source, 0x00000018) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx4Source, 0x00000019) && ok;
        ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegDsp1Rx5Source, 0x00000020) && ok;
    }

    ok = updateRegisterBits(amp, 0x00002018, 0x00000001, 0x00000000) && ok;

    ok = writeRegister(amp, 0x00000040, 0x00000055) && ok;
    ok = writeRegister(amp, 0x00000040, 0x000000AA) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegPowerControl1, 1, 0) && ok;
    ok = writeRegister(amp, 0x00007438, 0x00585941) && ok;
    ok = writeRegister(amp, 0x00007414, 0x08C82222) && ok;
    ok = writeRegister(amp, 0x0000742C, 0x00000009) && ok;
    ok = updateRegisterBits(amp, cirrus::devices::cs35l41::registers::kRegPowerControl2,
                            0x00003000 | cirrus::devices::cs35l41::registers::kMaskBoostEnable, 0) &&
         ok;
    ok = writeRegister(amp, 0x00000040, 0x000000CC) && ok;
    ok = writeRegister(amp, 0x00000040, 0x00000033) && ok;

    uint32_t digVol = 0, gainCtrl = 0;
    ok = readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &digVol) && ok;
    ok = readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &gainCtrl) && ok;
    CIRRUS_LOG("default volume states on %s: digital_volume=0x%08X gain=0x%08X", amp.name, digVol, gainCtrl);
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, 0x0000A678) && ok;
    ok = writeRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, 0x00000000) && ok;

    if (!ok) {
        amp.playbackFaulted = true;
        stopPlayback(amp);
    }

    snapshotDiagnostics(amp, "IDLE (POST-BOOT)");
    return ok;
}

bool CirrusAudioFixup::verifyIdleConfiguration(AmplifierState& amp) {
    uint32_t pwr1 = 0, pwr2 = 0, spEn = 0, spFmt = 0;
    uint32_t dacSrc = 0, digVol = 0, gain = 0;
    bool readable = readRegister(amp, cirrus::devices::cs35l41::registers::kRegPowerControl1, &pwr1, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegPowerControl2, &pwr2, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortEnables, &spEn, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegSerialPortFormat, &spFmt, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegDacPcm1Source, &dacSrc, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierDigitalVolumeControl, &digVol, TRACE_PROBE) &&
                    readRegister(amp, cirrus::devices::cs35l41::registers::kRegAmplifierGainControl, &gain, TRACE_PROBE);
    bool dspMode = amp.monitorCount >= 1;
    uint32_t expectedSpEn = 0x00010000U;
    uint32_t expectedDac = dspMode ? 0x00000032U : 0x00000008U;
    bool valid = readable && ((pwr1 & 0x1) == 0) && ((pwr2 & (0x00003001 | cirrus::devices::cs35l41::registers::kMaskBoostEnable)) == 0) &&
                 spEn == expectedSpEn && spFmt == 0x20200200U && dacSrc == expectedDac && digVol == 0x0000A678U && gain == 0;
    if (!valid) {
        CIRRUS_ERR("idle verify %s: read=%d pwr1=0x%08X pwr2=0x%08X sp_en=0x%08X sp_fmt=0x%08X dac=0x%08X vol=0x%08X gain=0x%08X mode=%s",
                   amp.name, readable, pwr1, pwr2, spEn, spFmt, dacSrc, digVol, gain, dspMode ? "DSP" : "BYPASS");
    }
    return valid;
}

bool CirrusAudioFixup::readOwnCalibration(AmplifierState& amp, OwnAmpCalibration& result) {
    char property[80];
    snprintf(property,sizeof(property),"ThinkBook_Calibration_Detail_%s",amp.name);
    auto status=[&](const char* reason) { setProperty(property,reason); };
    uint32_t id=0,revision=0;
    if (!readRegister(amp,0,&id) || !readRegister(amp,4,&revision)) {
        status("IDENTITY_READ_FAILED"); return false;
    }
    if (id!=0x35A40 || (revision&0xFF)!=0xB2) {
        status("UNEXPECTED_CHIP_OR_REVISION"); return false;
    }
    auto* options=IORegistryEntry::fromPath("IODeviceTree:/options");
    if (!options) { status("NVRAM_OPTIONS_NOT_READY"); return false; }
    auto* object=options->copyProperty("02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData");
    if (!object) object=options->copyProperty("CirrusSmartAmpCalibrationData");
    auto* bytes=OSDynamicCast(OSData,object);
    bool ok=false;
    if (!object) status("NVRAM_CALIBRATION_MISSING");
    else if (!bytes || bytes->getLength()!=48) status("NVRAM_CALIBRATION_TYPE_OR_LENGTH_INVALID");
    else {
        uint64_t uid=0;
        if (!mOwnCalibrationValidated) {
            uint32_t mapId=0,word22=0,word23=0;
            const bool readable=readRegister(amp,0x10,&mapId) &&
                readRegister(amp,0x458,&word22) && readRegister(amp,0x45C,&word23);
            if (!readable) status("RAW_OTP_READ_FAILED");
            else if (!ownUidFromOtp(mapId,word22,word23,uid)) status("RAW_OTP_UID_OR_MAP_INVALID");
            else {
                ok=matchOwnAmpCalibration(static_cast<const uint8_t*>(bytes->getBytesNoCopy()),bytes->getLength(),uid,result);
                status(ok ? "RAW_OTP_UID_MATCHED_FACTORY_RECORD" : "RAW_OTP_UID_NO_VALID_FACTORY_MATCH");
            }
        } else {
            // Initialization has now unpacked OTP. Recheck the actual DIE_STS UID
            // before applying calibration; never use the preflight result alone.
            uint32_t low=0,high=0;
            if (!readRegister(amp,0x17040,&low) || !readRegister(amp,0x17044,&high)) status("DIE_UID_READ_FAILED_AFTER_INIT");
            else {
                uid=(uint64_t(high)<<32)|low;
                ok=matchOwnAmpCalibration(static_cast<const uint8_t*>(bytes->getBytesNoCopy()),bytes->getLength(),uid,result);
                status(ok ? "DIE_UID_RECHECK_MATCHED_FACTORY_RECORD" : "DIE_UID_NO_VALID_FACTORY_MATCH_AFTER_INIT");
            }
        }
    }
    if (object) object->release();options->release();
    return ok;
}
bool CirrusAudioFixup::validateOwnCalibration() {
    mOwnCalibrationValidated=false;
    if (mAmpCount!=2 || mAmps[0].address!=0x40 || mAmps[1].address!=0x41) return false;
    OwnAmpCalibration calibration[2]{};
    for (size_t i=0;i<2;++i) {
        if (!readOwnCalibration(mAmps[i],calibration[i])) return false;
    }
    if (calibration[0].uid==calibration[1].uid) return false;
    mOwnCalibrationValidated=true;
    setProperty("ThinkBook_Calibration_Preflight", "TWO_EXACT_UID_MATCHES");
    setProperty("ThinkBook_NoCalibrationFallback", true);
    return true;
}

IOReturn CirrusAudioFixup::setProperties(OSObject* object) {
    if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    auto* dictionary=OSDynamicCast(OSDictionary,object);
    auto* action=dictionary?OSDynamicCast(OSString,dictionary->getObject("ThinkBookAction")):nullptr;
    if(!action||!mCommandGate)return kIOReturnBadArgument;
    uintptr_t operation=action->isEqualTo("probe")?1:action->isEqualTo("initialize")?2:
        action->isEqualTo("arm")?3:action->isEqualTo("stop")?4:0;
    if(!operation)return kIOReturnUnsupported;
    return mCommandGate->runAction(manualAction,reinterpret_cast<void*>(operation));
}
IOReturn CirrusAudioFixup::manualAction(OSObject* owner,void* operation,void*,void*,void*) {
    auto* self=OSDynamicCast(CirrusAudioFixup,owner);
    return self?self->runManualAction(reinterpret_cast<uintptr_t>(operation)):kIOReturnBadArgument;
}
IOReturn CirrusAudioFixup::runManualAction(uintptr_t operation) {
    if(mStopping||!mPowerAvailable)return kIOReturnOffline;
    if(operation==4) {
        if(mProbeTimer)mProbeTimer->cancelTimeout();
        bool safe=true;
        if(mManualArmed && mOwnCalibrationValidated)for(size_t i=0;i<mAmpCount;++i) {
            safe=stopPlayback(mAmps[i])&&safe;safe=stopDSP(mAmps[i])&&safe;
            mAmps[i].initialized=mAmps[i].firmwareValidated=false;
        }
        mManualArmed=false;mOwnCalibrationValidated=false;
        setProperty("ThinkBook_Control_Status",safe?"STOPPED_MANUALLY":"STOP_FAILED_FAULT_LATCHED");
        return safe?kIOReturnSuccess:kIOReturnIOError;
    }
    if(operation<1||operation>3)return kIOReturnBadArgument;
    if(mManualArmed) {
        if(operation==3) {
            for(size_t i=0;i<mAmpCount;++i)if(!mAmps[i].initialized||!mAmps[i].firmwareValidated||!mAmps[i].dspAlive)return kIOReturnNotReady;
            mProbeTimer->setTimeoutMS(250);
            setProperty("ThinkBook_Control_Status","ARMED_PROTECTED_PLAYBACK");
            return kIOReturnSuccess;
        }
        return kIOReturnBusy;
    }
    setProperty("ThinkBook_Control_Status","MANUAL_READ_ONLY_PREFLIGHT");
    mOwnCalibrationValidated=false;
    for(auto& amp:mAmps) {amp.playbackFaulted=false;amp.cleanupAttempts=0;}
    detectAmplifiers();
    if(!validateOwnCalibration()) {
        setProperty("ThinkBook_Control_Status","CALIBRATION_PREFLIGHT_FAILED_NO_WRITES");
        return kIOReturnNotReady;
    }
    if(operation==1) {
        setProperty("ThinkBook_Control_Status","READ_ONLY_PREFLIGHT_PASSED");
        mOwnCalibrationValidated=false;
        return kIOReturnSuccess;
    }
    mManualArmed=true;
    setProperty("ThinkBook_Control_Status","MANUAL_PROTECTED_DSP_INITIALIZATION");
    fullDriverFlow();
    bool ready=mAmpCount==2;
    for(size_t i=0;i<mAmpCount;++i)ready=ready&&mAmps[i].initialized&&mAmps[i].firmwareValidated&&mAmps[i].dspAlive&&!mAmps[i].playbackFaulted;
    if(!ready) {
        setProperty("ThinkBook_Control_Status","DSP_INITIALIZATION_FAILED_OUTPUT_DISABLED");
        return kIOReturnIOError;
    }
    setProperty("ThinkBook_Control_Status",operation==3?"ARMED_PROTECTED_PLAYBACK":"DSP_READY_MUTED_WAITING_ARM");
    if(operation==3)mProbeTimer->setTimeoutMS(250);
    return kIOReturnSuccess;
}

bool CirrusAudioFixup::internalSpeakersSelected() {
    if(!mHdaControllerCache)return false;
    if(!mSpeakerSelectorCache) {
        auto* iterator=IORegistryIterator::iterateOver(mHdaControllerCache,gIOServicePlane,kIORegistryIterateRecursively);
        if(!iterator)return false;
        while(auto* entry=iterator->getNextObject()) {
            if(!entry->metaCast("IOAudioSelectorControl"))continue;
            auto* subtype=OSDynamicCast(OSNumber,entry->getProperty("IOAudioControlSubType"));
            auto* usage=OSDynamicCast(OSNumber,entry->getProperty("IOAudioControlUsage"));
            if(subtype&&usage&&subtype->unsigned32BitValue()==0x6F757470U&&usage->unsigned32BitValue()==0x6F757470U) {
                mSpeakerSelectorCache=entry;entry->retain();break;
            }
        }
        iterator->release();
    }
    auto* value=mSpeakerSelectorCache?OSDynamicCast(OSNumber,mSpeakerSelectorCache->getProperty("IOAudioControlValue")):nullptr;
    const bool speakers=value&&value->unsigned32BitValue()==0x6973706BU;
    setProperty("ThinkBook_Internal_Speakers_Selected",speakers);
    return speakers;
}
