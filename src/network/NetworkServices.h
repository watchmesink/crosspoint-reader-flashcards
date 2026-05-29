#pragma once

#include "CrossPointWebServer.h"

namespace NetworkServices {

void loop();
void stop();
void stopWebServer();
void setSuspended(bool suspended);
bool ensureWebServerRunning();
bool isWebServerRunning();
bool preventAutoSleep();
bool wantsFastLoop();
CrossPointWebServer* webServer();
const char* hostname();

}  // namespace NetworkServices
