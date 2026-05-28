#include "WifiPower.h"

#include <WiFi.h>

#include "WifiCredentialStore.h"

namespace WifiPower {

bool isPoweredOn() { return WiFi.getMode() != WIFI_MODE_NULL; }

bool isStationEnabled() { return (WiFi.getMode() & WIFI_MODE_STA) != 0; }

bool hasConnection() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

void enableStation() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
}

void disable() {
  if (WiFi.getMode() & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (WiFi.getMode() & WIFI_MODE_STA) {
    WiFi.disconnect(false);
    delay(100);
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool connectSaved() {
  WIFI_STORE.loadFromFile();

  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) {
    return false;
  }

  const auto* cred = WIFI_STORE.findCredential(ssid);
  if (!cred) {
    return false;
  }

  enableStation();
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  return true;
}

std::string currentSsid() {
  if (!hasConnection()) {
    return "";
  }
  return WiFi.SSID().c_str();
}

std::string currentIp() {
  if (!hasConnection()) {
    return "";
  }
  return WiFi.localIP().toString().c_str();
}

}  // namespace WifiPower
