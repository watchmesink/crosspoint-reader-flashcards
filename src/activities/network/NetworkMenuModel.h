#pragma once

#include <string>
#include <vector>

enum class NetworkMode { WEB_UPLOAD, CONNECT_WIFI, DISABLE_WIFI, CONNECT_CALIBRE, CREATE_HOTSPOT };

namespace NetworkMenuModel {

std::vector<NetworkMode> menuModes(bool hasConnection, bool isPoweredOn);
std::string modeLabel(NetworkMode mode, bool hasConnection, bool isPoweredOn);
std::string modeDescription(NetworkMode mode, bool hasConnection);
std::string wifiStatusText(bool isPoweredOn, bool hasConnection, const std::string& ssid);

}  // namespace NetworkMenuModel
