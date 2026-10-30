#include <Arduino.h>
#include <USB.h>
#include <esp_timer.h>
#include <errno.h>
#include <atomic>
#include <math.h>
#include "config.h"
#include "usb_profile.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || ARDUINO_USB_MODE != 0
#error Select ESP32-S3 and USB Mode = USB-OTG (TinyUSB)
#endif
#if ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MSC_ON_BOOT || ARDUINO_USB_DFU_ON_BOOT
#error Disable USB CDC, MSC and DFU on boot for a mouse-only device
#endif

struct __attribute__((packed)) MouseReport {
  uint8_t buttons;
  int8_t wheel;
  int16_t dx, dy;
};
static_assert(sizeof(MouseReport) == 6, "HID report layout mismatch");
#include "circle_motion.h"
struct Event { uint32_t due; MouseReport report; };
Event events[MAX_EVENTS];
unsigned expected = 0, loaded = 0, cursor = 0;
bool running = false, releasePending = true;
std::atomic<bool> streaming{false};
uint32_t nextSequence = 0;
std::atomic<uint8_t> heldButtons{0};
SemaphoreHandle_t sent = nullptr;
SemaphoreHandle_t hidMutex = nullptr, stateMutex = nullptr, streamDone = nullptr;
QueueHandle_t streamQueue = nullptr;
TaskHandle_t streamTask = nullptr;
std::atomic<bool> streamSuccess{false};
// Faults are printed by the UART owner, never from the USB producer task.
std::atomic<unsigned> streamFault{0};
std::atomic<uint8_t> idleRate{0};
std::atomic<uint32_t> lastReport{0};
int64_t epoch = 0;
std::atomic<uint32_t> lastContact{0};
uint32_t worstLate = 0;
// Accessed under stateMutex, which is NEVER held across USB completion waits.
uint32_t reportCount = 0, intervalMin = UINT32_MAX, intervalMax = 0, missedSlots = 0;
int64_t firstCompletion = 0, lastCompletion = 0;
CircleMotion circleMotion;
bool circleReleasing = false;
char line[128];
unsigned lineLength = 0;
bool overflow = false;

extern "C" const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
  return instance == 0 ? REPORT_DESCRIPTOR : nullptr;
}
extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id,
    hid_report_type_t type, uint8_t *buffer, uint16_t length) {
  if (instance || id || type != HID_REPORT_TYPE_INPUT) return 0;
  // A state query must not replay the last relative displacement.
  MouseReport state = {heldButtons, 0, 0, 0};
  uint16_t count = min(length, uint16_t(tud_hid_n_get_protocol(0) == HID_PROTOCOL_BOOT ? 3 : 6));
  memcpy(buffer, &state, count);
  return count;
}
extern "C" void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t,
                                      const uint8_t *, uint16_t) {}
extern "C" bool tud_hid_set_idle_cb(uint8_t instance, uint8_t rate) {
  if (instance) return false;
  idleRate = rate;
  return true;
}
extern "C" void tud_hid_set_protocol_cb(uint8_t, uint8_t) {}
// Arduino-ESP32 3.3.8 / its pinned TinyUSB use uint16_t for the callback length.
extern "C" void tud_hid_report_complete_cb(uint8_t, const uint8_t *, uint16_t) {
  if (sent) xSemaphoreGive(sent);
}

bool sendMouseLocked(const MouseReport &report) {
  if (!sent || !tud_hid_n_ready(0)) return false;
  xSemaphoreTake(sent, 0);  // discard an acknowledgement from an earlier timeout
  // Boot protocol uses buttons, signed 8-bit X/Y; report protocol uses the
  // preserved six-byte layout: buttons, wheel, signed 16-bit X/Y.
  int8_t boot[3] = {int8_t(report.buttons), int8_t(constrain(int(report.dx), -127, 127)),
                   int8_t(constrain(int(report.dy), -127, 127))};
  bool bootMode = tud_hid_n_get_protocol(0) == HID_PROTOCOL_BOOT;
  if (!tud_hid_n_report(0, 0, bootMode ? static_cast<const void *>(boot) : &report,
                        bootMode ? 3 : sizeof(report))) return false;
  // A timeout can happen after transfer: callers abort and never retry motion.
  bool ok = xSemaphoreTake(sent, pdMS_TO_TICKS(30)) == pdTRUE;
  lastReport = millis();
  if (ok) heldButtons = report.buttons;
  return ok;
}

bool sendMouse(const MouseReport &report) {
  if (!hidMutex) return false;
  xSemaphoreTake(hidMutex, portMAX_DELAY);
  bool ok = sendMouseLocked(report);
  xSemaphoreGive(hidMutex);
  return ok;
}

void streamReports(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (streaming) {
      xSemaphoreTake(hidMutex, portMAX_DELAY);
      if (!streaming) { xSemaphoreGive(hidMutex); break; }
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      unsigned fault = digitalRead(STOP_PIN) == LOW ? 1 :
          uint32_t(millis() - lastContact.load()) > 250 ? 2 :
          (!tud_mounted() || tud_suspended()) ? 3 : 0;
      MouseReport report = {heldButtons, 0, 0, 0};
      bool requested = false;
      if (!fault) {
        requested = xQueueReceive(streamQueue, &report, 0) == pdTRUE;
        if (circleMotion.active && !requested) {
          report = circleMotion.next();
          circleReleasing = !circleMotion.active;
        }
      }
      xSemaphoreGive(stateMutex);
      // UART commands can update/query state while this transfer is pending.
      bool completed = !fault && sendMouseLocked(report);
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      if (!fault) {
        if (!completed) fault = 4;
        else {
          circleReleasing = false;
          int64_t now = esp_timer_get_time();
          if (reportCount) {
            uint32_t gap = now - lastCompletion;
            intervalMin = min(intervalMin, gap);
            intervalMax = max(intervalMax, gap);
            // Completion-task scheduling adds jitter; count gaps >= 1.5 ms.
            if (gap >= 1500) ++missedSlots;
          } else firstCompletion = now;
          lastCompletion = now;
          ++reportCount;
        }
      }
      if (requested || fault) {
        streamSuccess = requested && !fault;
        xSemaphoreGive(streamDone);
      }
      if (fault) { streamFault = fault; streaming = false; }
      xSemaphoreGive(stateMutex);
      xSemaphoreGive(hidMutex);
      // Let the equally-prioritized UART owner acquire the mutex for commands.
      taskYIELD();
      // No delay(1): the host's 1 ms interrupt endpoint already clocks USB.
      // sendMouseLocked blocks until completion, then primes the next poll.
    }
  }
}

uint16_t crc16(const char *s) {
  uint16_t crc = 0xFFFF;
  while (*s) {
    crc ^= uint16_t(uint8_t(*s++)) << 8;
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

void releaseButtons() {
  if (releasePending && tud_hid_n_ready(0)) {
    MouseReport report = {0, 0, 0, 0};
    if (sendMouse(report)) {
      heldButtons = 0;
      releasePending = false;
    }
  }
}

void stopRun(const char *reason) {
  running = false;
  streaming = false;
  if (hidMutex) {
    xSemaphoreTake(hidMutex, portMAX_DELAY);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    circleMotion.active = false;
    circleReleasing = false;
    xQueueReset(streamQueue);
    streamFault = 0;
    xSemaphoreGive(stateMutex);
    xSemaphoreGive(hidMutex);
  }
  expected = loaded = cursor = 0;
  releasePending = true;
  releaseButtons();
  if (reason) Serial0.println(reason);
}

// Parse exactly n decimal integers; reject overflow and trailing garbage.
bool numbers(char *s, int64_t *values, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    while (*s == ' ') ++s;
    if (!*s) return false;
    char *end;
    errno = 0;
    values[i] = strtoll(s, &end, 10);
    if (end == s || errno || (*end && *end != ' ')) return false;
    s = end;
  }
  while (*s == ' ') ++s;
  return !*s;
}

void command(char *s) {
  // Unframed STOP is always accepted, including during playback.
  if (!strcmp(s, "STOP")) { stopRun("OK STOP"); return; }
  char *star = strrchr(s, '*');
  if (!star || strlen(star + 1) != 4) { stopRun("ERR FRAME"); return; }
  for (unsigned i = 1; i <= 4; ++i)
    if (!isxdigit((unsigned char)star[i])) { stopRun("ERR FRAME"); return; }
  uint16_t received = strtoul(star + 1, nullptr, 16);
  *star = 0;
  if (crc16(s) != received) { stopRun("ERR CRC"); return; }
  lastContact = millis();
  if (!strcmp(s, "HELLO")) {
    Serial0.printf("OK HELLO 3 %u %u %u %u USB=%u\n", MAX_EVENTS,
                   MIN_GAP_US, MAX_DURATION_US, WATCHDOG_MS, tud_mounted() && !tud_suspended());
  } else if (!strcmp(s, "IDENTITY")) {
    Serial0.printf("OK IDENTITY VID=%04X PID=%04X RELEASE=%04X MFR=%s PRODUCT=%s SERIAL=%s EP_IN=81 SIZE=6 INTERVAL=1 BOOT=1 SPEED=full EP0=%u\n",
                   MOUSE_USB_VID, MOUSE_USB_PID, MOUSE_DEVICE_RELEASE, MANUFACTURER_NAME,
                   PRODUCT_NAME, MOUSE_USB_SERIAL, CFG_TUD_ENDPOINT0_SIZE);
  } else if (!strcmp(s, "CAPS")) {
    Serial0.println("OK CAPS STREAM=1 MAX_AXIS=32767 WATCHDOG_MS=250");
  } else if (!strcmp(s, "PACE")) {
    Serial0.println("OK PACE HZ=1000 SOURCE=USB");
  } else if (!strcmp(s, "STATS")) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    uint32_t count = reportCount, low = count > 1 ? intervalMin : 0;
    uint32_t high = intervalMax, gaps = missedSlots;
    int64_t elapsed = count > 1 ? lastCompletion - firstCompletion : 0;
    bool circle = circleMotion.active || circleReleasing;
    xSemaphoreGive(stateMutex);
    Serial0.printf("OK STATS REPORTS=%u ELAPSED_US=%lld MIN_US=%u MAX_US=%u GAPS=%u CIRCLE=%u\n",
                   count, elapsed, low, high, gaps, circle);
  } else if (!strcmp(s, "STREAM")) {
    stopRun(nullptr);
    if (!tud_hid_n_ready(0) || releasePending || digitalRead(STOP_PIN) == LOW) {
      stopRun("ERR NOT_READY"); return;
    }
    nextSequence = 0;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    reportCount = intervalMax = missedSlots = 0;
    intervalMin = UINT32_MAX;
    firstCompletion = lastCompletion = 0;
    streamFault = 0;
    xSemaphoreGive(stateMutex);
    streaming = true;
    xTaskNotifyGive(streamTask);
    Serial0.println("OK STREAM");
  } else if (!strncmp(s, "CIRCLE ", 7) && streaming) {
    int64_t v[4];
    // radius counts, period USB slots (1 ms each), turns, drawing button mask.
    if (!numbers(s + 7, v, 4) || v[0] < 1 || v[0] > 1000 ||
        v[1] < 500 || v[1] > 10000 || v[2] < 1 || v[2] > 20 ||
        v[1] * v[2] > 60000 || (v[3] != 0 && v[3] != 1)) { stopRun("ERR CIRCLE"); return; }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    circleMotion.start(v[0], v[1], v[2], v[3]);
    circleReleasing = false;
    xSemaphoreGive(stateMutex);
    Serial0.println("OK CIRCLE");
  } else if (!strncmp(s, "M ", 2) && streaming) {
    int64_t v[5];
    if (!numbers(s + 2, v, 5) || v[0] < 0 || v[0] > UINT32_MAX ||
        uint32_t(v[0]) != nextSequence || v[1] < -32767 || v[1] > 32767 ||
        v[2] < -32767 || v[2] > 32767 || v[3] < 0 || v[3] > 7 ||
        v[4] < -127 || v[4] > 127) {
      stopRun("ERR EVENT"); return;
    }
    if (digitalRead(STOP_PIN) == LOW || !tud_mounted() || tud_suspended()) {
      stopRun("ERR NOT_READY"); return;
    }
    MouseReport report = {uint8_t(v[3]), int8_t(v[4]), int16_t(v[1]), int16_t(v[2])};
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    bool circle = circleMotion.active || circleReleasing;
    xSemaphoreTake(streamDone, 0);
    streamSuccess = false;
    bool queued = !circle && xQueueSend(streamQueue, &report, 0) == pdTRUE;
    xSemaphoreGive(stateMutex);
    if (!queued || xSemaphoreTake(streamDone, pdMS_TO_TICKS(50)) != pdTRUE || !streamSuccess) {
      stopRun("ERR HID_SEND"); return;
    }
    Serial0.printf("OK M %u\n", nextSequence++);
  } else if (!strcmp(s, "PING")) {
    Serial0.println("OK PING");
  } else if (!strncmp(s, "LOAD ", 5) && !running && !streaming) {
    int64_t v[1];
    if (!numbers(s + 5, v, 1) || v[0] < 1 || v[0] > MAX_EVENTS) {
      stopRun("ERR COUNT"); return;
    }
    stopRun(nullptr);
    expected = v[0];
    Serial0.println("OK LOAD");
  } else if (!strncmp(s, "E ", 2) && !running) {
    int64_t v[6];
    if (!numbers(s + 2, v, 6) || !expected || loaded >= expected ||
        v[0] != loaded || v[1] < 0 || v[1] > MAX_DURATION_US ||
        (loaded && v[1] < events[loaded - 1].due + MIN_GAP_US) ||
        v[2] < -32767 || v[2] > 32767 || v[3] < -32767 || v[3] > 32767 ||
        v[4] < 0 || v[4] > 7 || v[5] < -127 || v[5] > 127) {
      stopRun("ERR EVENT"); return;
    }
    events[loaded] = {uint32_t(v[1]), {uint8_t(v[4]), int8_t(v[5]),
                                      int16_t(v[2]), int16_t(v[3])}};
    Serial0.printf("OK E %u\n", loaded++);
  } else if (!strcmp(s, "RUN") && !running) {
    releaseButtons();
    if (!expected || loaded != expected || events[loaded - 1].report.buttons ||
        !tud_hid_n_ready(0) || releasePending || digitalRead(STOP_PIN) == LOW) {
      stopRun("ERR NOT_READY"); return;
    }
    cursor = worstLate = 0;
    epoch = esp_timer_get_time() + 20000;  // time to return the RUN acknowledgement
    running = true;
    Serial0.println("OK RUN");
  } else {
    stopRun("ERR COMMAND");
  }
}

void setup() {
  pinMode(STOP_PIN, INPUT_PULLUP);
  Serial0.setRxBufferSize(2048);
  Serial0.begin(CONTROL_BAUD, SERIAL_8N1, CONTROL_RX_PIN, CONTROL_TX_PIN);
  USB.VID(MOUSE_USB_VID);
  USB.PID(MOUSE_USB_PID);
  USB.firmwareVersion(MOUSE_DEVICE_RELEASE);
  USB.productName(PRODUCT_NAME);
  USB.manufacturerName(MANUFACTURER_NAME);
  USB.usbClass(0); USB.usbSubClass(0); USB.usbProtocol(0);
  USB.serialNumber(MOUSE_USB_SERIAL);
  USB.usbAttributes(0xE0); USB.usbPower(100); USB.webUSB(false);
  sent = xSemaphoreCreateBinary();
  hidMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();
  streamDone = xSemaphoreCreateBinary();
  streamQueue = xQueueCreate(1, sizeof(MouseReport));
  if (!sent || !hidMutex || !stateMutex || !streamDone || !streamQueue || tinyusb_enable_interface(USB_INTERFACE_HID, sizeof(CONFIG_DESCRIPTOR) - 9,
                                        loadMouseInterface) != ESP_OK) {
    Serial0.println("ERR USB_INIT");
    return;
  }
  if (!USB.begin()) { Serial0.println("ERR USB_INIT"); return; }
  // Keep the producer on the UART task's core: taskYIELD then hands a released
  // mutex to that task instead of immediately reacquiring it on another core.
  if (xTaskCreatePinnedToCore(streamReports, "hid-stream", 4096, nullptr,
      uxTaskPriorityGet(nullptr), &streamTask, xPortGetCoreID()) != pdPASS) {
    Serial0.println("ERR TASK_INIT"); return;
  }
  lastContact = millis();
  Serial0.println("READY MOUSE_RELAY 3");
}

void loop() {
  if (!streamTask) { delay(100); return; }
  unsigned fault = streamFault.exchange(0);
  if (fault) {
    const char *reasons[] = {"", "ERR SWITCH", "ERR WATCHDOG", "ERR USB_DISCONNECTED", "ERR HID_SEND"};
    stopRun(reasons[fault]);
  }
  if (streaming) {
    if (digitalRead(STOP_PIN) == LOW) stopRun("ERR SWITCH");
    else if (uint32_t(millis() - lastContact) > 250) stopRun("ERR WATCHDOG");
    else if (!USB) stopRun("ERR USB_DISCONNECTED");
  }
  // Bounded input work prevents an incoming serial flood from starving motion.
  for (unsigned budget = 0; budget < 128 && Serial0.available(); ++budget) {
    char c = Serial0.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLength] = 0;
      if (overflow) stopRun("ERR LINE_TOO_LONG");
      else if (lineLength) command(line);
      lineLength = 0; overflow = false;
    } else if (!overflow && lineLength < sizeof(line) - 1) {
      line[lineLength++] = c;
    } else { overflow = true; }
  }
  releaseButtons();
  if (!running) {
    uint8_t rate = idleRate.load();
    if (!streaming && rate && !releasePending && tud_hid_n_ready(0) &&
        uint32_t(millis() - lastReport.load()) >= uint32_t(rate) * 4) {
      sendMouse({heldButtons, 0, 0, 0});
    }
    delay(1); return;
  }
  if (digitalRead(STOP_PIN) == LOW) { stopRun("ERR SWITCH"); return; }
  if (uint32_t(millis() - lastContact) > WATCHDOG_MS) {
    stopRun("ERR WATCHDOG"); return;
  }
  if (!USB) { stopRun("ERR USB_DISCONNECTED"); return; }
  int64_t elapsed = esp_timer_get_time() - epoch;
  if (elapsed < events[cursor].due) { delayMicroseconds(100); return; }
  uint32_t late = elapsed - events[cursor].due;
  if (late > MAX_LATENESS_US || !tud_hid_n_ready(0)) {
    stopRun("ERR USB_OR_LATE"); return;
  }
  worstLate = max(worstLate, late);
  heldButtons = events[cursor].report.buttons;
  if (!sendMouse(events[cursor].report)) {
    // Never retry displacement: a timeout may occur after transmission.
    stopRun("ERR HID_SEND"); return;
  }
  if (++cursor == loaded) {
    Serial0.printf("DONE %u %u\n", cursor, worstLate);
    running = false;
    expected = loaded = cursor = 0;
  }
}
