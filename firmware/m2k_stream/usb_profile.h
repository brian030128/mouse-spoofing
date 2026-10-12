#pragma once

#include "esp32-hal-tinyusb.h"
#include "config.h"

// Recovered byte-for-byte from the board's 2026-10-12 flash backup.
// Backup SHA256: 9cd63ec5584b3ce5e12f022a4a35782e9fc22066539968cc934a1362912a0e89
// Device @ 0x1a6e4, configuration @ 0x1a6f6, report @ 0x1a718.
static const uint8_t REPORT_DESCRIPTOR[] = {
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09,
  0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
  0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
  0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x38,
  0x15, 0x81, 0x25, 0x7F, 0x35, 0x81, 0x45, 0x7F,
  0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x09, 0x30,
  0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F,
  0x36, 0x01, 0x80, 0x46, 0xFF, 0x7F, 0x75, 0x10,
  0x95, 0x02, 0x81, 0x06, 0xC0
};
static_assert(sizeof(REPORT_DESCRIPTOR) == 69, "Update configuration report length");

// One boot mouse interface, no interface/configuration strings, one IN endpoint.
// Full-speed interrupt bInterval is in milliseconds. EP0 stays at the pinned
// stack's real packet size: merely advertising 8 here would not reconfigure it.
static const uint8_t CONFIG_DESCRIPTOR[] = {
  0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xE0, 0x32,
  0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x45, 0x00,
  0x07, 0x05, 0x81, 0x03, 0x06, 0x00, 0x01
};
static_assert(sizeof(CONFIG_DESCRIPTOR) == 34, "USB configuration length mismatch");

static const tusb_desc_device_t DEVICE_DESCRIPTOR = {
  .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = 0x0200, .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor = MOUSE_USB_VID, .idProduct = MOUSE_USB_PID,
  .bcdDevice = MOUSE_DEVICE_RELEASE,
  .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1
};

// Arduino-ESP32 3.3.8 exposes these as weak callbacks. Owning them in the
// sketch removes the generic wrapper without patching the installed toolchain.
extern "C" const uint8_t *tud_descriptor_device_cb() {
  return reinterpret_cast<const uint8_t *>(&DEVICE_DESCRIPTOR);
}
extern "C" const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
  return index == 0 ? CONFIG_DESCRIPTOR : nullptr;
}
extern "C" const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  static uint16_t descriptor[64];
  if (index == 0) {
    descriptor[0] = 0x0304; descriptor[1] = 0x0409;
    return descriptor;
  }
  if (langid != 0x0409 || index < 1 || index > 3) return nullptr;
  const char *s = index == 1 ? MANUFACTURER_NAME : index == 2 ? PRODUCT_NAME : MOUSE_USB_SERIAL;
  size_t length = min(strlen(s), size_t(63));
  descriptor[0] = uint16_t(0x0300 | (2 + 2 * length));
  for (size_t i = 0; i < length; ++i) descriptor[i + 1] = uint8_t(s[i]);
  return descriptor;
}

// Register the interface so the core initializes TinyUSB's HID driver. The
// host-visible configuration callback above has no generic TinyUSB strings.
static uint16_t loadMouseInterface(uint8_t *dst, uint8_t *itf) {
  memcpy(dst, CONFIG_DESCRIPTOR + 9, sizeof(CONFIG_DESCRIPTOR) - 9);
  ++*itf;
  return sizeof(CONFIG_DESCRIPTOR) - 9;
}
