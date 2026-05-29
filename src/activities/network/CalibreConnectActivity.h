#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "activities/ActivityWithSubactivity.h"

enum class CalibreConnectState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * CalibreConnectActivity starts the file transfer server in STA mode,
 * but renders Calibre-specific instructions instead of the web transfer UI.
 */
class CalibreConnectActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  CalibreConnectState state = CalibreConnectState::WIFI_SELECTION;
  const std::function<void()> onComplete;

  std::string connectedIP;
  std::string connectedSSID;
  unsigned long lastHandleClientTime = 0;
  size_t lastProgressReceived = 0;
  size_t lastProgressTotal = 0;
  std::string currentUploadName;
  std::string lastCompleteName;
  unsigned long lastCompleteAt = 0;
  bool exitRequested = false;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();

 public:
  explicit CalibreConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::function<void()>& onComplete)
      : ActivityWithSubactivity("CalibreConnect", renderer, mappedInput), onComplete(onComplete) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override;
};
