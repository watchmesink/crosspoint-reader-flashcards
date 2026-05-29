#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "NetworkMenuModel.h"
#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * NetworkModeSelectionActivity presents WiFi and network actions:
 * - "Show Web Upload URL" - Show the always-on upload server address
 * - "Turn WiFi On" - Turn on WiFi and connect to a saved or new network
 * - "Turn WiFi Off" - Turn WiFi and background web services off
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  bool updateRequired = false;
  const std::function<void(NetworkMode)> onModeSelected;
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  std::vector<NetworkMode> getMenuModes() const;
  NetworkMode modeAtIndex(int index) const;
  std::string getModeLabel(NetworkMode mode) const;
  std::string getModeDescription(NetworkMode mode) const;
  std::string getWifiStatusText() const;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void(NetworkMode)>& onModeSelected,
                                        const std::function<void()>& onCancel)
      : Activity("NetworkModeSelection", renderer, mappedInput), onModeSelected(onModeSelected), onCancel(onCancel) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
