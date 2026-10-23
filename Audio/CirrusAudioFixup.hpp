#pragma once
#include "Support/OwnCalibration.hpp"

#include "Core/RegisterIO.hpp"
#include "Devices/CS35L41/CS35L41Device.hpp"
#include "Devices/CS35L41/Hardware/OTPMap.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Diagnostics/DiagnosticTypes.hpp"
#include "Firmware/WMFW/FirmwareUploader.hpp"
#include "Platform/HDA/HDAController.hpp"
#include "Support/BitUtils.hpp"
#include "Support/Logging.hpp"

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSCollectionIterator.h>

#include <os/log.h>

#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"

struct VoodooI2CAddressedTransfer {
    uint8_t address;
    uint8_t* writeBuffer;
    uint16_t writeLength;
    uint8_t* readBuffer;
    uint16_t readLength;
};

struct RegisterSequence {
    uint32_t reg;
    uint32_t mask;
    uint32_t value;
    uint32_t delayUs;
    bool updateBits;
};

struct AmplifierState {
    const char* name{nullptr};
    uint8_t address{0};
    bool present{false};
    uint32_t deviceId{0};
    uint32_t revisionId{0};

    const uint8_t* wmfwData{nullptr};
    size_t wmfwSize{0};
    const uint8_t* binData{nullptr};
    size_t binSize{0};
    bool firmwareValidated{false};
    uint32_t finalCrc{0};

    unsigned int monitorCount{0};
    bool initialized{false};
    bool dspAlive{false};

    uint32_t lastIrq1Status1{0};
    uint32_t lastIrq1Status3{0};
    uint32_t lastPowerManagementStatus{0};
    uint32_t lastMailbox2{0};
    uint32_t lastStreamArbitrationError{0};
    uint32_t lastClockDetect{0};
    uint32_t lastStreamArbitrationControl{0};

    uint32_t lastTimestamp{0};
    bool playbackActive{false};
    uint32_t playbackStableCount{0};

    struct InterestingControl {
        char name[64]{};
        uint32_t address{0};
    } diagnosticControls[40]{};
    uint32_t diagnosticControlCount{0};
    uint32_t firmwareIdVersion{0};
    uint32_t monitorLogCountdown{0};
    uint64_t telemetrySequence{0};
    uint32_t haloStateRegister{0};
    uint32_t haloHeartbeatRegister{0};
    bool playbackFaulted{false};
    uint32_t cleanupAttempts{0};
    uint32_t pllRetryCooldown{0};
    cirrus::diagnostics::DiagnosticState diagnostic{};
};

struct FirmwareImage;

struct FirmwareResource {
    uint32_t subsystemVendor;
    uint32_t subsystemDevice;
    uint32_t spkid;
    const char* fwName;
    const char* binName;
    const uint8_t* wmfw;
    size_t wmfwSize;
    const uint8_t* bin;
    size_t binSize;
    const uint8_t* binRight;
    size_t binRightSize;
    bool isDummy;
};

class CirrusAudioFixup : public IOService {
    friend class FixupRegisterIOAdapter;
    bool readOwnCalibration(AmplifierState&, OwnAmpCalibration&);
    bool validateOwnCalibration();
    bool mOwnCalibrationValidated{false};
    bool mManualArmed{false};
    IOService* mHdaControllerCache{nullptr};
    IOMemoryMap* mHdaMapCache{nullptr};
    IORegistryEntry* mSpeakerSelectorCache{nullptr};
    bool internalSpeakersSelected();
    IOReturn runManualAction(uintptr_t);
    static IOReturn manualAction(OSObject*,void*,void*,void*,void*);

OSDeclareDefaultStructors(CirrusAudioFixup)

    public : bool init(OSDictionary* properties = nullptr) override;
    IOService* probe(IOService* provider, SInt32* score) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService* device) override;
    IOReturn setProperties(OSObject* properties) override;

private:
    void fullDriverFlow();

    void runBackgroundMonitor();

    IOService* mProvider{nullptr};
    IOWorkLoop* mWorkLoop{nullptr};
    IOTimerEventSource* mProbeTimer{nullptr};
    IOCommandGate* mCommandGate{nullptr};
    bool mPMInitialized{false};
    IOPMPowerState mPowerStates[2]{};
    bool mPowerAvailable{true};
    bool mStopping{false};
    bool mNeedsReinitialization{false};

    bool setupPowerManagement(IOService* provider);
    void handlePowerChange(bool powered);
    static IOReturn lifecycleAction(OSObject* owner, void* operation, void*, void*, void*);

    cirrus::platform::hda::HDAStreamState mHdaState;

    IOReturn mLastTransferReturn{kIOReturnSuccess};
    bool mCapturingFailureSnapshot{false};

    static constexpr size_t kMaxAmps = 4;
    size_t mAmpCount{2};
    AmplifierState mAmps[kMaxAmps]{{"left", cirrus::devices::cs35l41::registers::kI2cAddressLeft},
                                   {"right", cirrus::devices::cs35l41::registers::kI2cAddressRight},
                                   {"top_left", 0x42},
                                   {"top_right", 0x43}};

    cirrus::diagnostics::TraceStats mTraceStats{};
    void recordTransfer(cirrus::diagnostics::TraceSource, uint8_t, bool, bool, uint32_t, uint32_t, IOReturn);
    void publishStatistics();
    void setDiagnosticStage(AmplifierState& amp, cirrus::diagnostics::DriverStage stage);
    void markDiagnosticSuccess(AmplifierState& amp, cirrus::diagnostics::DriverStage stage);
    void recordDiagnosticFailure(AmplifierState& amp, cirrus::diagnostics::DiagnosticFailure failure, uint32_t reg = 0,
                                 uint32_t expected = 0, uint32_t actual = 0, IOReturn ioReturn = kIOReturnSuccess,
                                 bool captureSnapshot = true);

    void publishDriverVerdict();
    static const char* stageName(cirrus::diagnostics::DriverStage stage);
    static const char* failureName(cirrus::diagnostics::DiagnosticFailure failure);

    bool bootArgEnabled(const char* name);

    bool setupProbeTimer();

    size_t detectAmplifiers();

    bool mProbingAmplifiers{false};

    bool transferToAddress(uint8_t address, uint8_t* writeBuffer, uint16_t writeLength, uint8_t* readBuffer, uint16_t readLength);

public:
    bool bulkWrite(AmplifierState& amp, uint32_t reg, const uint8_t* data, size_t length,
                   cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool bulkRead(AmplifierState& amp, uint32_t reg, uint8_t* data, size_t length,
                  cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);

private:
    bool readRegister(AmplifierState& amp, uint32_t reg, uint32_t* value,
                      cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool writeRegister(AmplifierState& amp, uint32_t reg, uint32_t value,
                       cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool updateRegisterBits(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t value,
                            cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);
    bool pollRegisterBit(AmplifierState& amp, uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs,
                         cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_OTHER);

    bool sendMailboxCommand(AmplifierState& amp, uint32_t command, uint32_t expectedStatus);

    void snapshotDiagnostics(AmplifierState& amp, const char* stage);

    bool initCodec(AmplifierState& amp);

    bool unlockTestKey(AmplifierState& amp);
    bool lockTestKey(AmplifierState& amp);

    bool applyErrataPatch(AmplifierState& amp);

    bool unpackOTP(AmplifierState& amp);

    bool initializeHardwareErrata(AmplifierState& amp);

    bool configureHardware(AmplifierState& amp);

    bool synchronizeHdaStream();

    static bool supportedHdaFormat(uint16_t format);

    void discoverFirmware(AmplifierState& amp);

    bool bringupDSP(AmplifierState& amp);

    bool stopPlayback(AmplifierState& amp);
    bool checkProtectionStatus(AmplifierState& amp);
    bool verifyDSPAlive(AmplifierState& amp);

    bool parseDSPAlgorithms(AmplifierState& amp, FirmwareImage& outImage);
    bool stopDSP(AmplifierState& amp);

    bool applyCalibration(AmplifierState& amp, const FirmwareImage* image);
    void initializeFirmware(AmplifierState& amp);

    bool powerUpAmplifier(AmplifierState& amp);
    bool verifyIdleConfiguration(AmplifierState& amp);

    IOService* audioController();
    IOService* getAudioController();

    bool applyRegisterSequence(AmplifierState& amp, const RegisterSequence* sequence, size_t count);

    bool applyPLL(AmplifierState& amp);

    bool applyASP(AmplifierState& amp);

    bool applyGPIO(AmplifierState& amp);

    static void probeTimerFired(OSObject* owner, IOTimerEventSource* sender);
};
