#pragma once

// UART0: the USB-to-UART bridge on many S3 development boards.
// For an external 3.3 V adapter: adapter TX -> RX, RX -> TX, GND -> GND.
constexpr int CONTROL_RX_PIN = 44;
constexpr int CONTROL_TX_PIN = 43;
constexpr unsigned long CONTROL_BAUD = 115200;
// Ground this pin to abort. Change it if GPIO4 is occupied on your board.
constexpr int STOP_PIN = 4;
constexpr unsigned MAX_EVENTS = 4096;
constexpr unsigned MAX_DURATION_US = 60000000;
constexpr unsigned MIN_GAP_US = 10000;
constexpr unsigned WATCHDOG_MS = 1500;
constexpr unsigned MAX_LATENESS_US = 20000;

// Preserve the identity read from this board before the streaming update.
constexpr uint16_t MOUSE_USB_VID = 0x0483;
constexpr uint16_t MOUSE_USB_PID = 0xA3CF;
constexpr uint16_t MOUSE_DEVICE_RELEASE = 0x0200;
constexpr char PRODUCT_NAME[] = "M2K";
constexpr char MANUFACTURER_NAME[] = "Zaunkoenig";
constexpr char MOUSE_USB_SERIAL[] = "B9DD48BD1D1B firmware v1.3";
