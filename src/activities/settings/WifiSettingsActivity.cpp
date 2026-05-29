#include "WifiSettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiPower.h"

void WifiSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<WifiSettingsActivity*>(param);
  self->displayTaskLoop();
}

void WifiSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  WIFI_STORE.loadFromFile();
  updateRequired = true;

  xTaskCreate(&WifiSettingsActivity::taskTrampoline, "WifiSettingsTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void WifiSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void WifiSettingsActivity::openNetworkSelection(const bool autoConnect) {
  WifiPower::enableStation();
  SETTINGS.wifiEnabled = 1;
  SETTINGS.saveToFile();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); },
                                             autoConnect));
  xSemaphoreGive(renderingMutex);
}

void WifiSettingsActivity::onWifiSelectionComplete(const bool) {
  exitActivity();
  updateRequired = true;
}

void WifiSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    openNetworkSelection(!WifiPower::hasConnection());
    return;
  }

  if (WifiPower::isPoweredOn() && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    WifiPower::disable();
    SETTINGS.wifiEnabled = 0;
    SETTINGS.saveToFile();
    updateRequired = true;
  }
}

void WifiSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void WifiSettingsActivity::render() const {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Network", true, EpdFontFamily::BOLD);

  const auto pageHeight = renderer.getScreenHeight();
  const int top = pageHeight / 2 - 80;

  if (!WifiPower::isPoweredOn()) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, "WiFi Off", true, EpdFontFamily::BOLD);

    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      std::string savedInfo = "Saved: " + lastSsid;
      if (savedInfo.length() > 30) {
        savedInfo.replace(27, savedInfo.length() - 27, "...");
      }
      renderer.drawCenteredText(UI_10_FONT_ID, top + 45, savedInfo.c_str());
    }

    const auto labels = mappedInput.mapLabels("« Back", "Enable", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, top, "WiFi On", true, EpdFontFamily::BOLD);

  if (WifiPower::hasConnection()) {
    std::string ssidInfo = "Network: " + WifiPower::currentSsid();
    if (ssidInfo.length() > 30) {
      ssidInfo.replace(27, ssidInfo.length() - 27, "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, top + 45, ssidInfo.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, top + 75, ("IP: " + WifiPower::currentIp()).c_str());
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top + 45, "Not connected");
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      std::string savedInfo = "Saved: " + lastSsid;
      if (savedInfo.length() > 30) {
        savedInfo.replace(27, savedInfo.length() - 27, "...");
      }
      renderer.drawCenteredText(UI_10_FONT_ID, top + 75, savedInfo.c_str());
    }
  }

  const char* confirmLabel = WifiPower::hasConnection() ? "Networks" : "Connect";
  const auto labels = mappedInput.mapLabels("« Back", confirmLabel, "Disable", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
