#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiPower.h"

void NetworkModeSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<NetworkModeSelectionActivity*>(param);
  self->displayTaskLoop();
}

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&NetworkModeSelectionActivity::taskTrampoline, "NetworkModeTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void NetworkModeSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void NetworkModeSelectionActivity::loop() {
  const auto menuModes = getMenuModes();
  if (selectedIndex >= static_cast<int>(menuModes.size())) {
    selectedIndex = static_cast<int>(menuModes.size()) - 1;
    updateRequired = true;
  }

  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onModeSelected(modeAtIndex(selectedIndex));
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this, &menuModes] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuModes.size());
    updateRequired = true;
  });

  buttonNavigator.onPrevious([this, &menuModes] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuModes.size());
    updateRequired = true;
  });
}

void NetworkModeSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void NetworkModeSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "File Transfer", true, EpdFontFamily::BOLD);

  // Draw current WiFi state
  const std::string wifiStatus = getWifiStatusText();
  renderer.drawCenteredText(UI_10_FONT_ID, 50, wifiStatus.c_str());

  // Draw menu items centered on screen
  constexpr int itemHeight = 50;  // Height for each menu item (including description)
  const auto menuModes = getMenuModes();
  const int menuItemCount = static_cast<int>(menuModes.size());
  const int startY = (pageHeight - (menuItemCount * itemHeight)) / 2 + 10;

  for (int i = 0; i < menuItemCount; i++) {
    const int itemY = startY + i * itemHeight;
    const bool isSelected = (i == selectedIndex);
    const NetworkMode mode = menuModes[i];
    const std::string label = getModeLabel(mode);
    const std::string description = getModeDescription(mode);

    // Draw selection highlight (black fill) for selected item
    if (isSelected) {
      renderer.fillRect(20, itemY - 2, pageWidth - 40, itemHeight - 6);
    }

    // Draw text: black=false (white text) when selected (on black background)
    //            black=true (black text) when not selected (on white background)
    renderer.drawText(UI_10_FONT_ID, 30, itemY, label.c_str(), /*black=*/!isSelected);
    renderer.drawText(SMALL_FONT_ID, 30, itemY + 22, description.c_str(), /*black=*/!isSelected);
  }

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::vector<NetworkMode> NetworkModeSelectionActivity::getMenuModes() const {
  std::vector<NetworkMode> modes = {
      NetworkMode::WEB_UPLOAD,
      NetworkMode::CONNECT_WIFI,
      NetworkMode::CONNECT_CALIBRE,
      NetworkMode::CREATE_HOTSPOT,
  };

  if (WifiPower::isPoweredOn()) {
    modes.push_back(NetworkMode::DISABLE_WIFI);
  }

  return modes;
}

NetworkMode NetworkModeSelectionActivity::modeAtIndex(const int index) const {
  const auto modes = getMenuModes();
  if (index < 0 || index >= static_cast<int>(modes.size())) {
    return modes.front();
  }
  return modes[index];
}

std::string NetworkModeSelectionActivity::getModeLabel(const NetworkMode mode) const {
  switch (mode) {
    case NetworkMode::WEB_UPLOAD:
      return "Web Upload";
    case NetworkMode::CONNECT_WIFI:
      if (WifiPower::hasConnection()) {
        return "Change WiFi Network";
      }
      return WifiPower::isPoweredOn() ? "Connect WiFi" : "Enable & Connect WiFi";
    case NetworkMode::DISABLE_WIFI:
      return "Disable WiFi";
    case NetworkMode::CONNECT_CALIBRE:
      return "Connect to Calibre";
    case NetworkMode::CREATE_HOTSPOT:
      return "Create Hotspot";
  }
  return "";
}

std::string NetworkModeSelectionActivity::getModeDescription(const NetworkMode mode) const {
  switch (mode) {
    case NetworkMode::WEB_UPLOAD:
      return WifiPower::hasConnection() ? "Upload books over the current WiFi" : "Connect WiFi, then start upload";
    case NetworkMode::CONNECT_WIFI:
      return WifiPower::hasConnection() ? "Select another saved or nearby network" : "Use a saved or nearby network";
    case NetworkMode::DISABLE_WIFI:
      return "Turn WiFi off to save battery";
    case NetworkMode::CONNECT_CALIBRE:
      return "Use Calibre wireless device transfers";
    case NetworkMode::CREATE_HOTSPOT:
      return "Create a WiFi network others can join";
  }
  return "";
}

std::string NetworkModeSelectionActivity::getWifiStatusText() const {
  if (!WifiPower::isPoweredOn()) {
    return "WiFi: Off";
  }

  if (!WifiPower::hasConnection()) {
    return "WiFi: On, not connected";
  }

  std::string status = "WiFi: " + WifiPower::currentSsid();
  if (status.length() > 34) {
    status.replace(31, status.length() - 31, "...");
  }
  return status;
}
