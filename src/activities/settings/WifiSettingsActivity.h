#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

class WifiSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit WifiSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& goBack)
      : ActivityWithSubactivity("WifiSettings", renderer, mappedInput), goBack(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> goBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void openNetworkSelection(bool autoConnect);
  void onWifiSelectionComplete(bool connected);
};
