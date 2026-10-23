# Vendored source subset

Only translation units and transitive headers used by build.py are included. Upstream project files, scripts, binaries and documentation are omitted. This is a local, board-specific derivative, not an upstream release.

- VoodooI2C: hoaug-tran/VoodooI2C, snapshot 1a1ae9a26a74ec6b7e32a5d07f6a6ae9413bf298; original LICENSE.txt retained.
- VoodooGPIO headers: VoodooI2C/VoodooGPIO, snapshot 717ab2a40a0debd33ec8b35cebed7a4ced7dbfbc; LICENSE.md retained.
- VoodooInput headers: acidanthera/VoodooInput, snapshot d897813718c2ef9ad143b6a86240fe52dcb1693e; LICENSE.txt retained.

Local VoodooI2C modifications (2026-10-23): bounded polling with FIFO flow control, repeated START/final STOP, timeouts and interrupt mask disabled for the supported I2C7 controller. Modified or added files:

- `VoodooI2C/VoodooI2C/VoodooI2CController/VoodooI2CControllerDriver.cpp`
- `VoodooI2C/VoodooI2C/VoodooI2CController/ThinkBookPolledTransfer.hpp`
- `VoodooI2C/VoodooI2C/VoodooI2CController/VoodooI2CControllerDriver.hpp`

Resources/I2C-Info.plist supplies the board-specific match and transport version. These changes retain the VoodooI2C GPL terms.
