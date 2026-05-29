#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "activities/flashcards/FlashcardsModel.h"
#include "activities/network/NetworkMenuModel.h"
#include "network/WifiStatusLabel.h"

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
  }
}

bool containsMode(const std::vector<NetworkMode>& modes, const NetworkMode mode) {
  return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

void testFlashcardsFolderResolution() {
  const auto& candidates = FlashcardsModel::folderCandidates();
  require(!candidates.empty(), "flashcards folder candidate list is not empty");
  require(candidates.front() == "/~/flashcards", "USB-visible flashcards folder is preferred");
  require(containsMode(NetworkMenuModel::menuModes(false, false), NetworkMode::CONNECT_WIFI),
          "sanity check shared test helper");

  std::string selected = FlashcardsModel::findFlashcardsFolder([](const std::string& path) {
    return path == "/flashcards";
  });
  require(selected == "/flashcards", "selects root /flashcards when it is a directory");

  selected = FlashcardsModel::findFlashcardsFolder([](const std::string& path) {
    return path == "/~/flashcards";
  });
  require(selected == "/~/flashcards", "selects USB-visible /~/flashcards fallback");

  selected = FlashcardsModel::findFlashcardsFolder([](const std::string& path) {
    return path == "/Flashcards";
  });
  require(selected == "/Flashcards", "accepts common capitalized Flashcards folder");

  selected = FlashcardsModel::findFlashcardsFolder([](const std::string&) {
    return false;
  });
  require(selected == "/~/flashcards", "falls back to ~/flashcards for the user-facing error path");

  require(FlashcardsModel::isTxtFile("deck.txt"), "recognizes lowercase .txt files");
  require(FlashcardsModel::isTxtFile("deck.TXT"), "recognizes uppercase .TXT files");
  require(!FlashcardsModel::isTxtFile("deck.csv"), "rejects non-txt files");

  require(FlashcardsModel::entryFilePath("/~/flashcards", "deck.txt") == "/~/flashcards/deck.txt",
          "builds path for basename entries");
  require(FlashcardsModel::entryFilePath("/~/flashcards", "/~/flashcards/deck.txt") == "/~/flashcards/deck.txt",
          "keeps absolute paths returned by SdFat directory entries");
}

void testNetworkMenuOffState() {
  const auto modes = NetworkMenuModel::menuModes(false, false);
  require(!containsMode(modes, NetworkMode::WEB_UPLOAD), "web upload URL is hidden before WiFi connects");
  require(!containsMode(modes, NetworkMode::DISABLE_WIFI), "WiFi off state does not offer Turn WiFi Off");
  require(modes.front() == NetworkMode::CONNECT_WIFI, "WiFi off state starts with Turn WiFi On");
  require(NetworkMenuModel::modeLabel(NetworkMode::CONNECT_WIFI, false, false) == "Turn WiFi On",
          "WiFi off state has a clear Turn WiFi On action");
  require(NetworkMenuModel::wifiStatusText(false, false, "") == "WiFi: Off", "WiFi off status text");
}

void testNetworkMenuPoweredDisconnectedState() {
  const auto modes = NetworkMenuModel::menuModes(false, true);
  require(!containsMode(modes, NetworkMode::WEB_UPLOAD), "web upload URL is hidden until connected");
  require(containsMode(modes, NetworkMode::DISABLE_WIFI), "powered WiFi state offers Turn WiFi Off");
  require(NetworkMenuModel::modeLabel(NetworkMode::CONNECT_WIFI, false, true) == "Connect WiFi",
          "powered disconnected state has Connect WiFi action");
  require(NetworkMenuModel::modeLabel(NetworkMode::DISABLE_WIFI, false, true) == "Turn WiFi Off",
          "powered disconnected state has clear Turn WiFi Off action");
  require(NetworkMenuModel::wifiStatusText(true, false, "") == "WiFi: On, not connected",
          "powered disconnected status text");
}

void testNetworkMenuConnectedState() {
  const auto modes = NetworkMenuModel::menuModes(true, true);
  require(modes.front() == NetworkMode::WEB_UPLOAD, "connected state shows web upload URL first");
  require(containsMode(modes, NetworkMode::DISABLE_WIFI), "connected state offers Turn WiFi Off");
  require(NetworkMenuModel::modeLabel(NetworkMode::CONNECT_WIFI, true, true) == "Change WiFi Network",
          "connected state can change WiFi network");
  require(NetworkMenuModel::modeDescription(NetworkMode::WEB_UPLOAD, true) == "Web server is already running",
          "connected state communicates always-on web server");
  require(NetworkMenuModel::wifiStatusText(true, true, "HomeNet") == "WiFi: HomeNet", "connected status text");
}

void testNetworkStatusTruncation() {
  const std::string status =
      NetworkMenuModel::wifiStatusText(true, true, "VeryLongNetworkNameThatShouldNotOverflowTheMenuHeader");
  require(status.length() <= 34, "long SSID status is truncated");
  require(status.rfind("...", status.length() - 3) != std::string::npos, "truncated status ends with ellipsis");
}

void testWifiStatusLabel() {
  require(std::string(WifiStatusLabel::CONNECTED) == "Wi-Fi", "battery status uses text Wi-Fi label");
}

}  // namespace

int main() {
  testFlashcardsFolderResolution();
  testNetworkMenuOffState();
  testNetworkMenuPoweredDisconnectedState();
  testNetworkMenuConnectedState();
  testNetworkStatusTruncation();
  testWifiStatusLabel();

  std::cout << "All unit tests passed" << std::endl;
  return 0;
}
