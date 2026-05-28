#pragma once

#include <string>

namespace WifiPower {

bool isPoweredOn();
bool isStationEnabled();
bool hasConnection();
void enableStation();
void disable();
bool connectSaved();
std::string currentSsid();
std::string currentIp();

}  // namespace WifiPower
