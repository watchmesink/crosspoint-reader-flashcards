#include "NetworkServices.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <memory>

#include "WifiPower.h"

namespace NetworkServices {
namespace {
constexpr const char* HOSTNAME = "crosspoint";
constexpr unsigned long WIFI_CHECK_INTERVAL_MS = 2000;
constexpr int MAX_WEB_ITERATIONS = 80;

std::unique_ptr<CrossPointWebServer> staWebServer;
bool mdnsStarted = false;
bool servicesSuspended = false;
unsigned long lastWifiCheck = 0;
unsigned long lastHandleClientTime = 0;
}  // namespace

const char* hostname() { return HOSTNAME; }

CrossPointWebServer* webServer() { return staWebServer.get(); }

bool isWebServerRunning() { return staWebServer && staWebServer->isRunning(); }

bool preventAutoSleep() { return isWebServerRunning(); }

bool wantsFastLoop() { return isWebServerRunning(); }

void stopWebServer() {
  if (staWebServer) {
    staWebServer->stop();
    staWebServer.reset();
  }

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }

  lastHandleClientTime = 0;
}

void stop() { stopWebServer(); }

void setSuspended(const bool suspended) {
  servicesSuspended = suspended;
  if (servicesSuspended) {
    stopWebServer();
  }
}

bool ensureWebServerRunning() {
  if (servicesSuspended || !WifiPower::hasConnection()) {
    return false;
  }

  if (isWebServerRunning()) {
    return true;
  }

  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(HOSTNAME);
    Serial.printf("[%lu] [NET] mDNS %s: http://%s.local/\n", millis(), mdnsStarted ? "started" : "failed", HOSTNAME);
  }

  staWebServer.reset(new CrossPointWebServer());
  staWebServer->begin();

  if (!staWebServer->isRunning()) {
    Serial.printf("[%lu] [NET] Background web server failed to start\n", millis());
    staWebServer.reset();
    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
    }
    return false;
  }

  Serial.printf("[%lu] [NET] Background web server running at http://%s/\n", millis(),
                WifiPower::currentIp().c_str());
  return true;
}

void loop() {
  if (servicesSuspended) {
    return;
  }

  if (!WifiPower::hasConnection()) {
    if (staWebServer) {
      Serial.printf("[%lu] [NET] WiFi disconnected; stopping background web server\n", millis());
      stopWebServer();
    }
    return;
  }

  if (!ensureWebServerRunning()) {
    return;
  }

  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheck = millis();
    const int rssi = WiFi.RSSI();
    if (rssi < -75) {
      Serial.printf("[%lu] [NET] Warning: weak WiFi signal: %d dBm\n", millis(), rssi);
    }
  }

  const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
  if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 250) {
    Serial.printf("[%lu] [NET] WARNING: %lu ms gap since web server handleClient\n", millis(),
                  timeSinceLastHandleClient);
  }

  esp_task_wdt_reset();
  for (int i = 0; i < MAX_WEB_ITERATIONS && isWebServerRunning(); i++) {
    staWebServer->handleClient();
    if ((i & 0x0F) == 0x0F) {
      esp_task_wdt_reset();
    }
    if ((i & 0x1F) == 0x1F) {
      yield();
    }
  }
  lastHandleClientTime = millis();
}

}  // namespace NetworkServices
