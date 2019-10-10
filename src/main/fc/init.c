/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#include "blackbox/blackbox.h"

#include "build/build_config.h"
#include "build/debug.h"

#include "cms/cms.h"
#include "cms/cms_types.h"

#include "common/axis.h"
#include "common/color.h"
#include "common/maths.h"
#include "common/printf_serial.h"

#include "config/config_eeprom.h"
#include "config/feature.h"

#include "drivers/accgyro/accgyro.h"
#include "drivers/adc.h"
#include "drivers/bus.h"
#include "drivers/bus_i2c.h"
#include "drivers/bus_quadspi.h"
#include "drivers/bus_spi.h"
#include "drivers/buttons.h"
#include "drivers/camera_control.h"
#include "drivers/compass/compass.h"
#include "drivers/dma.h"
#include "drivers/exti.h"
#include "drivers/flash.h"
#include "drivers/inverter.h"
#include "drivers/io.h"
#include "drivers/light_led.h"
#include "drivers/mco.h"
#include "drivers/nvic.h"
#include "drivers/persistent.h"
#include "drivers/pin_pull_up_down.h"
#include "drivers/pwm_esc_detect.h"
#include "drivers/pwm_output.h"
#include "drivers/rx/rx_pwm.h"
#include "drivers/sensor.h"
#include "drivers/serial.h"
#include "drivers/serial_softserial.h"
#include "drivers/serial_uart.h"
#include "drivers/sdcard.h"
#include "drivers/sdio.h"
#include "drivers/sound_beeper.h"
#include "drivers/system.h"
#include "drivers/time.h"
#include "drivers/timer.h"
#include "drivers/transponder_ir.h"
#include "drivers/usb_io.h"
#ifdef USE_USB_MSC
#include "drivers/usb_msc.h"
#endif
#include "drivers/vtx_common.h"
#include "drivers/vtx_rtc6705.h"
#include "drivers/vtx_table.h"

#include "fc/board_info.h"
#include "fc/config.h"
#include "fc/dispatch.h"
#include "fc/init.h"
#include "fc/rc_controls.h"
#include "fc/runtime_config.h"
#include "fc/stats.h"
#include "fc/tasks.h"

#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/rpm_filter.h"
#include "flight/servos.h"

#include "io/asyncfatfs/asyncfatfs.h"
#include "io/beeper.h"
#include "io/dashboard.h"
#include "io/displayport_max7456.h"
#include "io/displayport_msp.h"
#include "io/displayport_srxl.h"
#include "io/flashfs.h"
#include "io/gimbal.h"
#include "io/gps.h"
#include "io/ledstrip.h"
#include "io/motors.h"
#include "io/pidaudio.h"
#include "io/piniobox.h"
#include "io/rcdevice_cam.h"
#include "io/serial.h"
#include "io/servos.h"
#include "io/transponder_ir.h"
#include "io/vtx.h"
#include "io/vtx_control.h"
#include "io/vtx_rtc6705.h"
#include "io/vtx_smartaudio.h"
#include "io/vtx_tramp.h"

#ifdef USE_PERSISTENT_MSC_RTC
#include "msc/usbd_storage.h"
#endif

#include "msp/msp.h"
#include "msp/msp_serial.h"

#include "osd/osd.h"

#include "pg/adc.h"
#include "pg/beeper.h"
#include "pg/beeper_dev.h"
#include "pg/bus_i2c.h"
#include "pg/bus_spi.h"
#include "pg/bus_quadspi.h"
#include "pg/flash.h"
#include "pg/mco.h"
#include "pg/motor.h"
#include "pg/pinio.h"
#include "pg/piniobox.h"
#include "pg/pin_pull_up_down.h"
#include "pg/pg.h"
#include "pg/rx.h"
#include "pg/rx_spi.h"
#include "pg/rx_pwm.h"
#include "pg/sdcard.h"
#include "pg/vcd.h"
#include "pg/vtx_io.h"

#include "rx/rx.h"
#include "rx/rx_spi.h"
#include "rx/spektrum.h"

#include "scheduler/scheduler.h"

#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/battery.h"
#include "sensors/boardalignment.h"
#include "sensors/compass.h"
#include "sensors/esc_sensor.h"
#include "sensors/gyro.h"
#include "sensors/initialisation.h"
#include "sensors/sensors.h"

#include "telemetry/telemetry.h"

#ifdef USE_HARDWARE_REVISION_DETECTION
#include "hardware_revision.h"
#endif

#ifdef TARGET_PREINIT
void targetPreInit(void);
#endif

#ifdef SOFTSERIAL_LOOPBACK
serialPort_t *loopbackPort;
#endif

uint8_t systemState = SYSTEM_STATE_INITIALISING;

void processLoopback(void)
{
#ifdef SOFTSERIAL_LOOPBACK
    if (loopbackPort) {
        uint8_t bytesWaiting;
        while ((bytesWaiting = serialRxBytesWaiting(loopbackPort))) {
            uint8_t b = serialRead(loopbackPort);
            serialWrite(loopbackPort, b);
        };
    }
#endif
}

#ifdef BUS_SWITCH_PIN
void busSwitchInit(void)
{
static IO_t busSwitchResetPin        = IO_NONE;

    busSwitchResetPin = IOGetByTag(IO_TAG(BUS_SWITCH_PIN));
    IOInit(busSwitchResetPin, OWNER_SYSTEM, 0);
    IOConfigGPIO(busSwitchResetPin, IOCFG_OUT_PP);

    // ENABLE
    IOLo(busSwitchResetPin);
}
#endif

static void configureSPIAndQuadSPI(void)
{
#ifdef USE_SPI
    spiPinConfigure(spiPinConfig(0));
#endif

    sensorsPreInit();

#ifdef USE_SPI
    spiPreinit();

#ifdef USE_SPI_DEVICE_1
    spiInit(SPIDEV_1);
#endif
#ifdef USE_SPI_DEVICE_2
    spiInit(SPIDEV_2);
#endif
#ifdef USE_SPI_DEVICE_3
    spiInit(SPIDEV_3);
#endif
#ifdef USE_SPI_DEVICE_4
    spiInit(SPIDEV_4);
#endif
#ifdef USE_SPI_DEVICE_5
    spiInit(SPIDEV_5);
#endif
#ifdef USE_SPI_DEVICE_6
    spiInit(SPIDEV_6);
#endif
#endif // USE_SPI

#ifdef USE_QUADSPI
    quadSpiPinConfigure(quadSpiConfig(0));

#ifdef USE_QUADSPI_DEVICE_1
    quadSpiInit(QUADSPIDEV_1);
#endif
#endif // USE_QUAD_SPI
}

void sdCardAndFSInit()
{
    sdcard_init(sdcardConfig());
    afatfs_init();
}


void init(void)
{
#ifdef SERIAL_PORT_COUNT
    printfSerialInit();
#endif

    systemInit();

    // initialize IO (needed for all IO operations)
    IOInitGlobal();

#ifdef USE_HARDWARE_REVISION_DETECTION
    detectHardwareRevision();
#endif

#ifdef USE_BRUSHED_ESC_AUTODETECT
    // Opportunistically use the first motor pin of the default configuration for detection.
    // We are doing this as with some boards, timing seems to be important, and the later detection will fail.
    ioTag_t motorIoTag = timerioTagGetByUsage(TIM_USE_MOTOR, 0);

    if (motorIoTag) {
        detectBrushedESC(motorIoTag);
    }
#endif

    enum {
        FLASH_INIT_ATTEMPTED            = (1 << 0),
        SD_INIT_ATTEMPTED               = (1 << 1),
        SPI_AND_QSPI_INIT_ATTEMPTED      = (1 << 2),
    };
    uint8_t initFlags = 0;


#ifdef CONFIG_IN_SDCARD

    //
    // Config in sdcard presents an issue with pin configuration since the pin and sdcard configs for the
    // sdcard are in the config which is on the sdcard which we can't read yet!
    //
    // FIXME We need to add configuration somewhere, e.g. bootloader image or reserved flash area, that can be read by the firmware.
    // it's currently possible for the firmware resource allocation to be wrong after the config is loaded if the user changes the settings.
    // This would cause undefined behaviour once the config is loaded.  so for now, users must NOT change sdio/spi configs needed for
    // the system to boot and/or to save the config.
    //
    // note that target specific SDCARD/SDIO/SPI/QUADSPI configs are
    // also not supported in USE_TARGET_CONFIG/targetConfigure() when using CONFIG_IN_SDCARD.
    //

    //
    // IMPORTANT: all default flash and pin configurations must be valid for the target after pgResetAll() is called.
    // Target designers must ensure other devices connected the same SPI/QUADSPI interface as the flash chip do not
    // cause communication issues with the flash chip.  e.g. use external pullups on SPI/QUADSPI CS lines.
    //

#ifdef TARGET_BUS_INIT
#error "CONFIG_IN_SDCARD and TARGET_BUS_INIT are mutually exclusive"
#endif

    pgResetAll();

#if defined(STM32H7) && defined(USE_SDCARD_SDIO) // H7 only for now, likely should be applied to F4/F7 too
    sdioPinConfigure();
    SDIO_GPIO_Init();
#endif
#ifdef USE_SDCARD_SPI
    configureSPIAndQuadSPI();
    initFlags |= SPI_AND_QSPI_INIT_ATTEMPTED;
#endif

    sdCardAndFSInit();
    initFlags |= SD_INIT_ATTEMPTED;

    while (afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY) {
        afatfs_poll();

        if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL) {
            failureMode(FAILURE_SDCARD_INITIALISATION_FAILED);
        }
    }

#endif // CONFIG_IN_SDCARD

#ifdef CONFIG_IN_EXTERNAL_FLASH
    //
    // Config on external flash presents an issue with pin configuration since the pin and flash configs for the
    // external flash are in the config which is on a chip which we can't read yet!
    //
    // FIXME We need to add configuration somewhere, e.g. bootloader image or reserved flash area, that can be read by the firmware.
    // it's currently possible for the firmware resource allocation to be wrong after the config is loaded if the user changes the settings.
    // This would cause undefined behaviour once the config is loaded.  so for now, users must NOT change flash/pin configs needed for
    // the system to boot and/or to save the config.
    //
    // note that target specific FLASH/SPI/QUADSPI configs are
    // also not supported in USE_TARGET_CONFIG/targetConfigure() when using CONFIG_IN_EXTERNAL_FLASH.
    //

    //
    // IMPORTANT: all default flash and pin configurations must be valid for the target after pgResetAll() is called.
    // Target designers must ensure other devices connected the same SPI/QUADSPI interface as the flash chip do not
    // cause communication issues with the flash chip.  e.g. use external pullups on SPI/QUADSPI CS lines.
    //
    pgResetAll();

#ifdef TARGET_BUS_INIT
#error "CONFIG_IN_EXTERNAL_FLASH and TARGET_BUS_INIT are mutually exclusive"
#endif

    configureSPIAndQuadSPI();
    initFlags |= SPI_AND_QSPI_INIT_ATTEMPTED;


#ifndef USE_FLASH_CHIP
#error "CONFIG_IN_EXTERNAL_FLASH requires USE_FLASH_CHIP to be defined."
#endif

    bool haveFlash = flashInit(flashConfig());

    if (!haveFlash) {
        failureMode(FAILURE_EXTERNAL_FLASH_INIT_FAILED);
    }
    initFlags |= FLASH_INIT_ATTEMPTED;

#endif // CONFIG_IN_EXTERNAL_FLASH

    initEEPROM();

    ensureEEPROMStructureIsValid();

    bool readSuccess = readEEPROM();

#if defined(USE_BOARD_INFO)
    initBoardInformation();
#endif

    if (!readSuccess || !isEEPROMVersionValid() || strncasecmp(systemConfig()->boardIdentifier, TARGET_BOARD_IDENTIFIER, sizeof(TARGET_BOARD_IDENTIFIER))) {
        resetEEPROM(false);
    }

    systemState |= SYSTEM_STATE_CONFIG_LOADED;

#ifdef USE_BRUSHED_ESC_AUTODETECT
    // Now detect again with the actually configured pin for motor 1, if it is not the default pin.
    ioTag_t configuredMotorIoTag = motorConfig()->dev.ioTags[0];

    if (configuredMotorIoTag && configuredMotorIoTag != motorIoTag) {
        detectBrushedESC(configuredMotorIoTag);
    }
#endif

    debugMode = systemConfig()->debug_mode;

#ifdef TARGET_PREINIT
    targetPreInit();
#endif

#if !defined(USE_FAKE_LED)
    ledInit(statusLedConfig());
#endif
    LED2_ON;

#ifdef USE_EXTI
    EXTIInit();
#endif

#if defined(USE_BUTTONS)

    buttonsInit();

    delayMicroseconds(10);  // allow configuration to settle // XXX Could be removed, too?

    // Allow EEPROM reset with two-button-press without power cycling in DEBUG build
#ifdef DEBUG
#define EEPROM_RESET_PRECONDITION true
#else
#define EEPROM_RESET_PRECONDITION (!isMPUSoftReset())
#endif

    if (EEPROM_RESET_PRECONDITION) {
#if defined(BUTTON_A_PIN) && defined(BUTTON_B_PIN)
        // two buttons required
        uint8_t secondsRemaining = 5;
        bool bothButtonsHeld;
        do {
            bothButtonsHeld = buttonAPressed() && buttonBPressed();
            if (bothButtonsHeld) {
                if (--secondsRemaining == 0) {
                    resetEEPROM(false);
#ifdef USE_PERSISTENT_OBJECTS
                    persistentObjectWrite(PERSISTENT_OBJECT_RESET_REASON, RESET_NONE);
#endif
                    systemReset();
                }
                delay(1000);
                LED0_TOGGLE;
            }
        } while (bothButtonsHeld);
#endif
    }

#undef EEPROM_RESET_PRECONDITION

#endif // USE_BUTTONS

    // Note that spektrumBind checks if a call is immediately after
    // hard reset (including power cycle), so it should be called before
    // systemClockSetHSEValue and OverclockRebootIfNecessary, as these
    // may cause soft reset which will prevent spektrumBind not to execute
    // the bind procedure.

#if defined(USE_SPEKTRUM_BIND)
    if (featureIsEnabled(FEATURE_RX_SERIAL)) {
        switch (rxConfig()->serialrx_provider) {
        case SERIALRX_SPEKTRUM1024:
        case SERIALRX_SPEKTRUM2048:
        case SERIALRX_SRXL:
            // Spektrum satellite binding if enabled on startup.
            // Must be called before that 100ms sleep so that we don't lose satellite's binding window after startup.
            // The rest of Spektrum initialization will happen later - via spektrumInit()
            spektrumBind(rxConfigMutable());
            break;
        }
    }
#endif

#ifdef STM32F4
    // Only F4 has non-8MHz boards
    systemClockSetHSEValue(systemConfig()->hseMhz * 1000000U);
#endif

#ifdef USE_OVERCLOCK
    OverclockRebootIfNecessary(systemConfig()->cpu_overclock);
#endif

    // Configure MCO output after config is stable
#ifdef USE_MCO
    mcoInit(mcoConfig());
#endif

#ifdef USE_TIMER
    timerInit();  // timer must be initialized before any channel is allocated
#endif

#ifdef BUS_SWITCH_PIN
    busSwitchInit();
#endif

#if defined(USE_UART)
    uartPinConfigure(serialPinConfig());
#endif

#if defined(AVOID_UART1_FOR_PWM_PPM)
    serialInit(featureIsEnabled(FEATURE_SOFTSERIAL),
            featureIsEnabled(FEATURE_RX_PPM) || featureIsEnabled(FEATURE_RX_PARALLEL_PWM) ? SERIAL_PORT_USART1 : SERIAL_PORT_NONE);
#elif defined(AVOID_UART2_FOR_PWM_PPM)
    serialInit(featureIsEnabled(FEATURE_SOFTSERIAL),
            featureIsEnabled(FEATURE_RX_PPM) || featureIsEnabled(FEATURE_RX_PARALLEL_PWM) ? SERIAL_PORT_USART2 : SERIAL_PORT_NONE);
#elif defined(AVOID_UART3_FOR_PWM_PPM)
    serialInit(featureIsEnabled(FEATURE_SOFTSERIAL),
            featureIsEnabled(FEATURE_RX_PPM) || featureIsEnabled(FEATURE_RX_PARALLEL_PWM) ? SERIAL_PORT_USART3 : SERIAL_PORT_NONE);
#else
    serialInit(featureIsEnabled(FEATURE_SOFTSERIAL), SERIAL_PORT_NONE);
#endif

    mixerInit(mixerConfig()->mixerMode);
    mixerConfigureOutput();

    uint16_t idlePulse = motorConfig()->mincommand;
    if (featureIsEnabled(FEATURE_3D)) {
        idlePulse = flight3DConfig()->neutral3d;
    }
    if (motorConfig()->dev.motorPwmProtocol == PWM_TYPE_BRUSHED) {
        idlePulse = 0; // brushed motors
    }
#ifdef USE_MOTOR
    /* Motors needs to be initialized soon as posible because hardware initialization
     * may send spurious pulses to esc's causing their early initialization. Also ppm
     * receiver may share timer with motors so motors MUST be initialized here. */
    motorDevInit(&motorConfig()->dev, idlePulse, getMotorCount());
    systemState |= SYSTEM_STATE_MOTORS_READY;
#else
    UNUSED(idlePulse);
#endif

    if (0) {}
#if defined(USE_PPM)
    else if (featureIsEnabled(FEATURE_RX_PPM)) {
        ppmRxInit(ppmConfig());
    }
#endif
#if defined(USE_PWM)
    else if (featureIsEnabled(FEATURE_RX_PARALLEL_PWM)) {
        pwmRxInit(pwmConfig());
    }
#endif

#ifdef USE_BEEPER
    beeperInit(beeperDevConfig());
#endif
/* temp until PGs are implemented. */
#if defined(USE_INVERTER) && !defined(SIMULATOR_BUILD)
    initInverters(serialPinConfig());
#endif


#ifdef TARGET_BUS_INIT
    targetBusInit();

#else

    // Depending on compilation options SPI/QSPI initialisation may already be done.
    if (!(initFlags & SPI_AND_QSPI_INIT_ATTEMPTED)) {
        configureSPIAndQuadSPI();
        initFlags |= SPI_AND_QSPI_INIT_ATTEMPTED;
    }

#ifdef USE_USB_MSC
/* MSC mode will start after init, but will not allow scheduler to run,
 *  so there is no bottleneck in reading and writing data */
    mscInit();
    if (mscCheckBoot() || mscCheckButton()) {
        if (mscStart() == 0) {
             mscWaitForButton();
        } else {
            systemResetFromMsc();
        }
    }
#endif

#ifdef USE_PERSISTENT_MSC_RTC
    // if we didn't enter MSC mode then clear the persistent RTC value
    persistentObjectWrite(PERSISTENT_OBJECT_RTC_HIGH, 0);
    persistentObjectWrite(PERSISTENT_OBJECT_RTC_LOW, 0);
#endif

#ifdef USE_I2C
    i2cHardwareConfigure(i2cConfig(0));

    // Note: Unlike UARTs which are configured when client is present,
    // I2C buses are initialized unconditionally if they are configured.

#ifdef USE_I2C_DEVICE_1
    i2cInit(I2CDEV_1);
#endif
#ifdef USE_I2C_DEVICE_2
    i2cInit(I2CDEV_2);
#endif
#ifdef USE_I2C_DEVICE_3
    i2cInit(I2CDEV_3);
#endif
#ifdef USE_I2C_DEVICE_4
    i2cInit(I2CDEV_4);
#endif
#endif // USE_I2C

#endif // TARGET_BUS_INIT

#ifdef USE_HARDWARE_REVISION_DETECTION
    updateHardwareRevision();
#endif

#if defined(STM32H7) && defined(USE_SDCARD_SDIO) // H7 only for now, likely should be applied to F4/F7 too
    if (!(initFlags & SD_INIT_ATTEMPTED)) {
        sdioPinConfigure();
        SDIO_GPIO_Init();
    }
#endif


#ifdef USE_VTX_RTC6705
    bool useRTC6705 = rtc6705IOInit(vtxIOConfig());
#endif

#ifdef USE_CAMERA_CONTROL
    cameraControlInit();
#endif

// XXX These kind of code should goto target/config.c?
// XXX And these no longer work properly as FEATURE_RANGEFINDER does control HCSR04 runtime configuration.
#if defined(RANGEFINDER_HCSR04_SOFTSERIAL2_EXCLUSIVE) && defined(USE_RANGEFINDER_HCSR04) && defined(USE_SOFTSERIAL2)
    if (featureIsEnabled(FEATURE_RANGEFINDER) && featureIsEnabled(FEATURE_SOFTSERIAL)) {
        serialRemovePort(SERIAL_PORT_SOFTSERIAL2);
    }
#endif

#if defined(RANGEFINDER_HCSR04_SOFTSERIAL1_EXCLUSIVE) && defined(USE_RANGEFINDER_HCSR04) && defined(USE_SOFTSERIAL1)
    if (featureIsEnabled(FEATURE_RANGEFINDER) && featureIsEnabled(FEATURE_SOFTSERIAL)) {
        serialRemovePort(SERIAL_PORT_SOFTSERIAL1);
    }
#endif

#ifdef USE_ADC
    adcConfigMutable()->vbat.enabled = (batteryConfig()->voltageMeterSource == VOLTAGE_METER_ADC);
    adcConfigMutable()->current.enabled = (batteryConfig()->currentMeterSource == CURRENT_METER_ADC);

    // The FrSky D SPI RX sends RSSI_ADC_PIN (if configured) as A2
    adcConfigMutable()->rssi.enabled = featureIsEnabled(FEATURE_RSSI_ADC);
#ifdef USE_RX_SPI
    adcConfigMutable()->rssi.enabled |= (featureIsEnabled(FEATURE_RX_SPI) && rxSpiConfig()->rx_spi_protocol == RX_SPI_FRSKY_D);
#endif
    adcInit(adcConfig());
#endif

    initBoardAlignment(boardAlignment());

    if (!sensorsAutodetect()) {
        // if gyro was not detected due to whatever reason, notify and don't arm.
        if (true
#if defined(USE_UNIFIED_TARGET)
            && isSystemConfigured()
#endif
            ) {
            indicateFailure(FAILURE_MISSING_ACC, 2);
        }
        setArmingDisabled(ARMING_DISABLED_NO_GYRO);
    }

    systemState |= SYSTEM_STATE_SENSORS_READY;

    // gyro.targetLooptime set in sensorsAutodetect(),
    // so we are ready to call validateAndFixGyroConfig(), pidInit(), and setAccelerationFilter()
    validateAndFixGyroConfig();
    pidInit(currentPidProfile);
#ifdef USE_ACC
    accInitFilters();
#endif

#ifdef USE_PID_AUDIO
    pidAudioInit();
#endif

#ifdef USE_SERVOS
    servosInit();
    servoConfigureOutput();
    if (isMixerUsingServos()) {
        //pwm_params.useChannelForwarding = featureIsEnabled(FEATURE_CHANNEL_FORWARDING);
        servoDevInit(&servoConfig()->dev);
    }
    servosFilterInit();
#endif

#ifdef USE_PINIO
    pinioInit(pinioConfig());
#endif

#ifdef USE_PIN_PULL_UP_DOWN
    pinPullupPulldownInit();
#endif

#ifdef USE_PINIOBOX
    pinioBoxInit(pinioBoxConfig());
#endif

    LED1_ON;
    LED0_OFF;
    LED2_OFF;

    for (int i = 0; i < 10; i++) {
        LED1_TOGGLE;
        LED0_TOGGLE;
#if defined(USE_BEEPER)
        delay(25);
        if (!(beeperConfig()->beeper_off_flags & BEEPER_GET_FLAG(BEEPER_SYSTEM_INIT))) {
            BEEP_ON;
        }
        delay(25);
        BEEP_OFF;
#else
        delay(50);
#endif
    }
    LED0_OFF;
    LED1_OFF;

    imuInit();

    mspInit();
    mspSerialInit();

    failsafeInit();

    rxInit();

/*
 * CMS, display devices and OSD
 */
#ifdef USE_CMS
    cmsInit();
#endif

#if (defined(USE_OSD) || (defined(USE_MSP_DISPLAYPORT) && defined(USE_CMS)))
    displayPort_t *osdDisplayPort = NULL;
#endif

#if defined(USE_OSD)
    //The OSD need to be initialised after GYRO to avoid GYRO initialisation failure on some targets

    if (featureIsEnabled(FEATURE_OSD)) {
#if defined(USE_MAX7456)
        // If there is a max7456 chip for the OSD then use it
        osdDisplayPort = max7456DisplayPortInit(vcdProfile());
#elif defined(USE_CMS) && defined(USE_MSP_DISPLAYPORT) && defined(USE_OSD_OVER_MSP_DISPLAYPORT) // OSD over MSP; not supported (yet)
        osdDisplayPort = displayPortMspInit();
#endif
        // osdInit  will register with CMS by itself.
        osdInit(osdDisplayPort);
    }
#endif

#if defined(USE_CMS) && defined(USE_MSP_DISPLAYPORT)
    // If BFOSD is not active, then register MSP_DISPLAYPORT as a CMS device.
    if (!osdDisplayPort)
        cmsDisplayPortRegister(displayPortMspInit());
#endif

#ifdef USE_DASHBOARD
    // Dashbord will register with CMS by itself.
    if (featureIsEnabled(FEATURE_DASHBOARD)) {
        dashboardInit();
    }
#endif

#if defined(USE_CMS) && defined(USE_SPEKTRUM_CMS_TELEMETRY) && defined(USE_TELEMETRY_SRXL)
    // Register the srxl Textgen telemetry sensor as a displayport device
    cmsDisplayPortRegister(displayPortSrxlInit());
#endif

#ifdef USE_GPS
    if (featureIsEnabled(FEATURE_GPS)) {
        gpsInit();
    }
#endif

#ifdef USE_LED_STRIP
    ledStripInit();

    if (featureIsEnabled(FEATURE_LED_STRIP)) {
        ledStripEnable();
    }
#endif

#ifdef USE_TELEMETRY
    if (featureIsEnabled(FEATURE_TELEMETRY)) {
        telemetryInit();
    }
#endif

#ifdef USE_ESC_SENSOR
    if (featureIsEnabled(FEATURE_ESC_SENSOR)) {
        escSensorInit();
    }
#endif

#ifdef USE_USB_DETECT
    usbCableDetectInit();
#endif

#ifdef USE_TRANSPONDER
    if (featureIsEnabled(FEATURE_TRANSPONDER)) {
        transponderInit();
        transponderStartRepeating();
        systemState |= SYSTEM_STATE_TRANSPONDER_ENABLED;
    }
#endif

#ifdef USE_FLASH_CHIP
    if (!(initFlags & FLASH_INIT_ATTEMPTED)) {
        flashInit(flashConfig());
        initFlags |= FLASH_INIT_ATTEMPTED;
    }
#endif
#ifdef USE_FLASHFS
    flashfsInit();
#endif

#ifdef USE_BLACKBOX
#ifdef USE_SDCARD
    if (blackboxConfig()->device == BLACKBOX_DEVICE_SDCARD) {
        if (sdcardConfig()->mode) {
            if (!(initFlags & SD_INIT_ATTEMPTED)) {
                initFlags |= SD_INIT_ATTEMPTED;
                sdCardAndFSInit();
            }
        } else {
            blackboxConfigMutable()->device = BLACKBOX_DEVICE_NONE;
        }
    }
#endif
    blackboxInit();
#endif

#ifdef USE_ACC
    if (mixerConfig()->mixerMode == MIXER_GIMBAL) {
        accSetCalibrationCycles(CALIBRATING_ACC_CYCLES);
    }
#endif
    gyroStartCalibration(false);
#ifdef USE_BARO
    baroSetCalibrationCycles(CALIBRATING_BARO_CYCLES);
#endif

#if defined(USE_VTX_COMMON) || defined(USE_VTX_CONTROL)
    vtxTableInit();
#endif

#ifdef USE_VTX_CONTROL
    vtxControlInit();

#if defined(USE_VTX_COMMON)
    vtxCommonInit();
#endif

#ifdef USE_VTX_SMARTAUDIO
    vtxSmartAudioInit();
#endif

#ifdef USE_VTX_TRAMP
    vtxTrampInit();
#endif

#ifdef USE_VTX_RTC6705
    if (!vtxCommonDevice() && useRTC6705) { // external VTX takes precedence when configured.
        vtxRTC6705Init();
    }
#endif

#endif // VTX_CONTROL

#ifdef USE_TIMER
    // start all timers
    // TODO - not implemented yet
    timerStart();
#endif

    ENABLE_STATE(SMALL_ANGLE);

#ifdef SOFTSERIAL_LOOPBACK
    // FIXME this is a hack, perhaps add a FUNCTION_LOOPBACK to support it properly
    loopbackPort = (serialPort_t*)&(softSerialPorts[0]);
    if (!loopbackPort->vTable) {
        loopbackPort = openSoftSerial(0, NULL, 19200, SERIAL_NOT_INVERTED);
    }
    serialPrint(loopbackPort, "LOOPBACK\r\n");
#endif

    batteryInit(); // always needs doing, regardless of features.

#ifdef USE_DASHBOARD
    if (featureIsEnabled(FEATURE_DASHBOARD)) {
#ifdef USE_OLED_GPS_DEBUG_PAGE_ONLY
        dashboardShowFixedPage(PAGE_GPS);
#else
        dashboardResetPageCycling();
        dashboardEnablePageCycling();
#endif
    }
#endif

#ifdef USE_RCDEVICE
    rcdeviceInit();
#endif // USE_RCDEVICE

#ifdef USE_MOTOR
    motorPostInit();
    motorEnable();
#endif

#ifdef USE_PERSISTENT_STATS
    statsInit();
#endif

    setArmingDisabled(ARMING_DISABLED_BOOT_GRACE_TIME);

    fcTasksInit();

    systemState |= SYSTEM_STATE_READY;
}
