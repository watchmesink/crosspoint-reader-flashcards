#include "FlashcardsWebSync.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <HardwareSerial.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "WifiPower.h"
#include "util/UrlUtils.h"

namespace FlashcardsWebSync {
namespace {

constexpr char CONFIG_PATH[] = "/.crosspoint/flashcards_sync.json";
constexpr char PROGRESS_DIR[] = "/.crosspoint";
constexpr unsigned long CONNECT_SETTLE_MS = 4000;
constexpr unsigned long RETRY_AFTER_ERROR_MS = 5 * 60 * 1000;
constexpr size_t MAX_DECK_FILE_BYTES = 256 * 1024;
constexpr const char* DECKS[] = {"german", "ukrainian", "english"};

struct Config {
  std::string url;
  std::string token;
  bool enabled = false;
};

bool wasConnected = false;
unsigned long connectedSinceMs = 0;
bool attemptedThisConnection = false;
unsigned long retryAtMs = 0;
volatile bool syncing = false;
TaskHandle_t syncTaskHandle = nullptr;
std::string summary;

bool loadConfig(Config& config) {
  FsFile file;
  if (!Storage.openFileForRead("FSY", CONFIG_PATH, file)) {
    return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.printf("[%lu] [FSY] Config parse error: %s\n", millis(), err.c_str());
    return false;
  }
  config.url = doc["url"] | "";
  config.token = doc["token"] | "";
  config.enabled = doc["enabled"] | true;
  while (!config.url.empty() && config.url.back() == '/') {
    config.url.pop_back();
  }
  return config.enabled && !config.url.empty();
}

std::unique_ptr<WiFiClient> makeClient(const std::string& url) {
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secure = new WiFiClientSecure();
    secure->setInsecure();
    return std::unique_ptr<WiFiClient>(secure);
  }
  return std::unique_ptr<WiFiClient>(new WiFiClient());
}

void addCommonHeaders(HTTPClient& http, const Config& config) {
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!config.token.empty()) {
    http.addHeader("Authorization", ("Bearer " + config.token).c_str());
  }
}

// One-shot request with a string body and string response.
int requestString(const Config& config, const char* method, const std::string& url, const char* contentType,
                  const std::string& body, std::string& outResponse) {
  auto client = makeClient(url);
  HTTPClient http;
  if (!http.begin(*client, url.c_str())) {
    return -1;
  }
  http.setTimeout(20000);
  addCommonHeaders(http, config);
  if (contentType != nullptr) {
    http.addHeader("Content-Type", contentType);
  }
  const int code = http.sendRequest(method, reinterpret_cast<uint8_t*>(const_cast<char*>(body.data())), body.size());
  if (code > 0) {
    outResponse = http.getString().c_str();
  }
  http.end();
  return code;
}

int downloadToFile(const Config& config, const std::string& url, const std::string& destPath) {
  auto client = makeClient(url);
  HTTPClient http;
  if (!http.begin(*client, url.c_str())) {
    return -1;
  }
  http.setTimeout(20000);
  addCommonHeaders(http, config);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    const std::string tmpPath = destPath + ".fsy";
    if (Storage.exists(tmpPath.c_str())) {
      Storage.remove(tmpPath.c_str());
    }
    FsFile file;
    if (!Storage.openFileForWrite("FSY", tmpPath, file)) {
      http.end();
      return -2;
    }
    http.writeToStream(&file);
    file.close();
    if (Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
    if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
      http.end();
      return -3;
    }
  }
  http.end();
  return code;
}

bool readFileBytes(const std::string& path, std::string& out, const size_t maxBytes) {
  FsFile file;
  if (!Storage.openFileForRead("FSY", path, file)) {
    return false;
  }
  const size_t size = file.size();
  if (size > maxBytes) {
    file.close();
    return false;
  }
  out.resize(size);
  const size_t got = file.read(reinterpret_cast<uint8_t*>(&out[0]), size);
  file.close();
  return got == size;
}

bool writeFileBytes(const std::string& path, const std::string& data) {
  const std::string tmpPath = path + ".fsy";
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("FSY", tmpPath, file)) {
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  file.close();
  if (written != data.size()) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
  return Storage.rename(tmpPath.c_str(), path.c_str());
}

std::string sha256HexOfFile(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("FSY", path, file)) {
    return "";
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buffer[1024];
  while (true) {
    const int got = file.read(buffer, sizeof(buffer));
    if (got <= 0) {
      break;
    }
    mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(got));
  }
  file.close();
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  return std::string(hex, 64);
}

std::string urlEncodeSegment(const std::string& value) {
  static const char* hexDigits = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    const bool unreserved =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~", c) != nullptr;
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hexDigits[c >> 4]);
      out.push_back(hexDigits[c & 0x0f]);
    }
  }
  return out;
}

// Response bodies are {"bin":"<base64>"} or {"bin":null}; extract without a
// full JSON parse to keep peak heap low (the payload can be ~30 KB).
bool extractBinField(const std::string& response, std::string& outBase64) {
  const size_t keyPos = response.find("\"bin\"");
  if (keyPos == std::string::npos) {
    return false;
  }
  const size_t colon = response.find(':', keyPos);
  if (colon == std::string::npos) {
    return false;
  }
  size_t pos = colon + 1;
  while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\t')) {
    pos++;
  }
  if (pos < response.size() && response[pos] == '"') {
    const size_t end = response.find('"', pos + 1);
    if (end == std::string::npos) {
      return false;
    }
    outBase64 = response.substr(pos + 1, end - pos - 1);
    return true;
  }
  outBase64.clear();  // null -> nothing to write back
  return true;
}

bool isTxtName(const char* name) {
  if (name[0] == '.') {
    return false;
  }
  const size_t len = strlen(name);
  if (len < 5) {
    return false;
  }
  return strcasecmp(name + len - 4, ".txt") == 0;
}

std::vector<std::string> listLocalDeckFiles(const std::string& folderPath) {
  std::vector<std::string> names;
  auto root = Storage.open(folderPath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    return names;
  }
  root.rewindDirectory();
  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (!file.isDirectory() && isTxtName(name)) {
      names.emplace_back(name);
    }
    file.close();
  }
  root.close();
  return names;
}

bool syncDeckProgress(const Config& config, const char* deck, int& actions) {
  const std::string binPath = std::string(PROGRESS_DIR) + "/flashcards_" + deck + ".bin";

  std::string body = "{\"bin\":";
  std::string localBin;
  if (readFileBytes(binPath, localBin, 64 * 1024) && !localBin.empty()) {
    const String encoded = base64::encode(reinterpret_cast<const uint8_t*>(localBin.data()), localBin.size());
    body += "\"";
    body += encoded.c_str();
    body += "\"";
  } else {
    localBin.clear();
    body += "null";
  }
  body += "}";

  std::string response;
  const std::string url = config.url + "/api/sync/" + deck + "/progress";
  const int code = requestString(config, "POST", url, "application/json", body, response);
  if (code != HTTP_CODE_OK) {
    Serial.printf("[%lu] [FSY] %s progress sync failed: HTTP %d\n", millis(), deck, code);
    return false;
  }

  std::string mergedBase64;
  if (!extractBinField(response, mergedBase64)) {
    Serial.printf("[%lu] [FSY] %s progress response unparseable\n", millis(), deck);
    return false;
  }
  response.clear();

  if (mergedBase64.empty()) {
    return true;  // no progress anywhere yet
  }

  std::string merged;
  merged.resize((mergedBase64.size() / 4 + 1) * 3);
  size_t decodedLen = 0;
  const int rc = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&merged[0]), merged.size(), &decodedLen,
                                       reinterpret_cast<const unsigned char*>(mergedBase64.data()),
                                       mergedBase64.size());
  if (rc != 0) {
    Serial.printf("[%lu] [FSY] %s merged bin base64 decode failed\n", millis(), deck);
    return false;
  }
  merged.resize(decodedLen);

  if (merged == localBin) {
    return true;
  }
  Storage.mkdir(PROGRESS_DIR);
  if (!writeFileBytes(binPath, merged)) {
    Serial.printf("[%lu] [FSY] %s failed writing merged progress\n", millis(), deck);
    return false;
  }
  actions++;
  Serial.printf("[%lu] [FSY] %s progress updated from web (%u bytes)\n", millis(), deck,
                static_cast<unsigned>(merged.size()));
  return true;
}

bool syncDeckFiles(const Config& config, const char* deck, int& actions) {
  const std::string manifestUrl = config.url + "/api/sync/" + deck + "/manifest";
  std::string response;
  const int code = requestString(config, "GET", manifestUrl, nullptr, "", response);
  if (code != HTTP_CODE_OK) {
    Serial.printf("[%lu] [FSY] %s manifest failed: HTTP %d\n", millis(), deck, code);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.printf("[%lu] [FSY] %s manifest parse error: %s\n", millis(), deck, err.c_str());
    return false;
  }
  response.clear();

  const std::string folderPath = std::string("/flashcards/") + deck;
  const std::vector<std::string> localFiles = listLocalDeckFiles(folderPath);

  struct WebFile {
    std::string name;
    std::string sha256;
  };
  std::vector<WebFile> webFiles;
  for (JsonObject f : doc["files"].as<JsonArray>()) {
    WebFile wf;
    wf.name = f["name"] | "";
    wf.sha256 = f["sha256"] | "";
    if (!wf.name.empty() && isTxtName(wf.name.c_str())) {
      webFiles.push_back(wf);
    }
  }
  std::vector<std::string> tombstones;
  for (JsonVariant t : doc["tombstones"].as<JsonArray>()) {
    const std::string name = t | "";
    if (!name.empty()) {
      tombstones.push_back(name);
    }
  }

  const auto isLocal = [&localFiles](const std::string& name) {
    return std::find(localFiles.begin(), localFiles.end(), name) != localFiles.end();
  };
  const auto isTombstoned = [&tombstones](const std::string& name) {
    return std::find(tombstones.begin(), tombstones.end(), name) != tombstones.end();
  };
  const auto webFileFor = [&webFiles](const std::string& name) -> const WebFile* {
    for (const auto& wf : webFiles) {
      if (wf.name == name) {
        return &wf;
      }
    }
    return nullptr;
  };

  bool ok = true;

  // Web deletions win: drop local files the web has tombstoned.
  for (const auto& name : tombstones) {
    if (isLocal(name)) {
      const std::string path = folderPath + "/" + name;
      if (Storage.remove(path.c_str())) {
        actions++;
        Serial.printf("[%lu] [FSY] %s deleted %s (removed on web)\n", millis(), deck, name.c_str());
      }
    }
  }

  // Download web-only files; re-download is skipped when content matches.
  bool folderEnsured = false;
  for (const auto& wf : webFiles) {
    const std::string path = folderPath + "/" + wf.name;
    const bool present = isLocal(wf.name);
    if (present && sha256HexOfFile(path) == wf.sha256) {
      continue;
    }
    if (present) {
      // Content differs: device wins, upload below.
      continue;
    }
    if (!folderEnsured) {
      Storage.mkdir(folderPath.c_str(), true);
      folderEnsured = true;
    }
    const std::string fileUrl = config.url + "/api/decks/" + deck + "/files/" + urlEncodeSegment(wf.name);
    const int dlCode = downloadToFile(config, fileUrl, path);
    if (dlCode == HTTP_CODE_OK) {
      actions++;
      Serial.printf("[%lu] [FSY] %s downloaded %s\n", millis(), deck, wf.name.c_str());
    } else {
      ok = false;
      Serial.printf("[%lu] [FSY] %s download of %s failed: %d\n", millis(), deck, wf.name.c_str(), dlCode);
    }
  }

  // Upload device-only files (unless deleted on web) and content mismatches.
  for (const auto& name : localFiles) {
    if (isTombstoned(name)) {
      continue;  // handled above
    }
    const WebFile* wf = webFileFor(name);
    const std::string path = folderPath + "/" + name;
    if (wf != nullptr && sha256HexOfFile(path) == wf->sha256) {
      continue;
    }
    std::string content;
    if (!readFileBytes(path, content, MAX_DECK_FILE_BYTES)) {
      Serial.printf("[%lu] [FSY] %s skipping oversized/unreadable %s\n", millis(), deck, name.c_str());
      continue;
    }
    const std::string fileUrl = config.url + "/api/decks/" + deck + "/files/" + urlEncodeSegment(name);
    std::string putResponse;
    const int putCode = requestString(config, "PUT", fileUrl, "text/plain; charset=utf-8", content, putResponse);
    if (putCode == HTTP_CODE_OK) {
      actions++;
      Serial.printf("[%lu] [FSY] %s uploaded %s (%u bytes)\n", millis(), deck, name.c_str(),
                    static_cast<unsigned>(content.size()));
    } else {
      ok = false;
      Serial.printf("[%lu] [FSY] %s upload of %s failed: %d\n", millis(), deck, name.c_str(), putCode);
    }
  }

  return ok;
}

void syncTask(void* param) {
  (void)param;
  Config config;
  bool allOk = true;
  int actions = 0;

  Serial.printf("[%lu] [FSY] Sync pass starting (heap %d)\n", millis(), ESP.getFreeHeap());
  for (const char* deck : DECKS) {
    if (!WifiPower::hasConnection()) {
      allOk = false;
      break;
    }
    if (!loadConfig(config)) {
      allOk = false;
      break;
    }
    if (!syncDeckFiles(config, deck, actions)) {
      allOk = false;
    }
    if (!syncDeckProgress(config, deck, actions)) {
      allOk = false;
    }
  }

  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%s, %d change%s", allOk ? "ok" : "errors", actions, actions == 1 ? "" : "s");
  summary = buffer;
  Serial.printf("[%lu] [FSY] Sync pass done: %s (heap %d)\n", millis(), summary.c_str(), ESP.getFreeHeap());

  if (!allOk) {
    retryAtMs = millis() + RETRY_AFTER_ERROR_MS;
  }

  syncing = false;
  syncTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

bool configPresent() { return Storage.exists(CONFIG_PATH); }

}  // namespace

void loopTick() {
  const bool connected = WifiPower::hasConnection();
  if (!connected) {
    wasConnected = false;
    attemptedThisConnection = false;
    connectedSinceMs = 0;
    return;
  }

  if (!wasConnected) {
    wasConnected = true;
    connectedSinceMs = millis();
  }

  if (syncing || syncTaskHandle != nullptr) {
    return;
  }

  const bool retryDue = retryAtMs != 0 && millis() >= retryAtMs;
  if (attemptedThisConnection && !retryDue) {
    return;
  }
  if (!attemptedThisConnection && millis() - connectedSinceMs < CONNECT_SETTLE_MS) {
    return;
  }

  attemptedThisConnection = true;
  retryAtMs = 0;

  if (!configPresent()) {
    return;
  }

  syncing = true;
  if (xTaskCreate(&syncTask, "FcdWebSync", 12288, nullptr, 1, &syncTaskHandle) != pdPASS) {
    syncing = false;
    syncTaskHandle = nullptr;
    Serial.printf("[%lu] [FSY] Failed to start sync task\n", millis());
  }
}

bool isSyncing() { return syncing; }

std::string lastSummary() { return summary; }

}  // namespace FlashcardsWebSync
