#pragma once

#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

namespace waiting_server {

// Returns true if the onboard BOOTSEL button is currently pressed.
// Must execute strictly from RAM (not flash) because it temporarily floats QSPI Flash CS.
inline bool __no_inline_not_in_flash_func(is_bootsel_pressed)(void) {
    const uint CS_PIN_INDEX = 1;

    // Must disable interrupts, as interrupt handlers may be in flash!
    uint32_t flags = save_and_disable_interrupts();

    // Set chip select to Hi-Z (float)
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Short delay while CS floats (in RAM)
    for (int i = 0; i < 1000; ++i) {
        tight_loop_contents();
    }

    // Read the SIO QSPI CS pin: button pulls the pin LOW when pressed.
#if defined(__ARM_ARCH_6M__) // RP2040 (Cortex-M0+)
    #define CS_BIT (1u << 1)
#else // RP2350
    #define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool pin_high = (sio_hw->gpio_hi_in & CS_BIT) != 0;
#undef CS_BIT

    // Restore the state of chip select before re-enabling interrupts and flash access!
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);

    // Pin is active-low: low = pressed, high = released
    return !pin_high;
}

} // namespace waiting_server
