#include "NetworkMenuModel.h"

namespace NetworkMenuModel {

std::vector<NetworkMode> menuModes(const bool hasConnection, const bool isPoweredOn) {
  std::vector<NetworkMode> modes;

  if (hasConnection) {
    modes.push_back(NetworkMode::WEB_UPLOAD);
  }

  modes.push_back(NetworkMode::CONNECT_WIFI);

  if (isPoweredOn) {
    modes.push_back(NetworkMode::DISABLE_WIFI);
  }

  modes.push_back(NetworkMode::CONNECT_CALIBRE);
  modes.push_back(NetworkMode::CREATE_HOTSPOT);

  return modes;
}

std::string modeLabel(const NetworkMode mode, const bool hasConnection, const bool isPoweredOn) {
  switch (mode) {
    case NetworkMode::WEB_UPLOAD:
      return "Show Web Upload URL";
    case NetworkMode::CONNECT_WIFI:
      if (hasConnection) {
        return "Change WiFi Network";
      }
      return isPoweredOn ? "Connect WiFi" : "Turn WiFi On";
    case NetworkMode::DISABLE_WIFI:
      return "Turn WiFi Off";
    case NetworkMode::CONNECT_CALIBRE:
      return "Connect to Calibre";
    case NetworkMode::CREATE_HOTSPOT:
      return "Create Hotspot";
  }
  return "";
}

std::string modeDescription(const NetworkMode mode, const bool hasConnection) {
  switch (mode) {
    case NetworkMode::WEB_UPLOAD:
      return "Web server is already running";
    case NetworkMode::CONNECT_WIFI:
      return hasConnection ? "Select another saved or nearby network" : "Use a saved or nearby network";
    case NetworkMode::DISABLE_WIFI:
      return "Stop WiFi and web services";
    case NetworkMode::CONNECT_CALIBRE:
      return "Use Calibre wireless device transfers";
    case NetworkMode::CREATE_HOTSPOT:
      return "Create a WiFi network others can join";
  }
  return "";
}

std::string wifiStatusText(const bool isPoweredOn, const bool hasConnection, const std::string& ssid) {
  if (!isPoweredOn) {
    return "WiFi: Off";
  }

  if (!hasConnection) {
    return "WiFi: On, not connected";
  }

  std::string status = "WiFi: " + ssid;
  if (status.length() > 34) {
    status.replace(31, status.length() - 31, "...");
  }
  return status;
}

}  // namespace NetworkMenuModel
