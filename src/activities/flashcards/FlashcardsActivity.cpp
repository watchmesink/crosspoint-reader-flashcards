#include "FlashcardsActivity.h"

#include <GfxRenderer.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "FlashcardsModel.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr uint8_t FLASHCARD_PROGRESS_FILE_VERSION = 5;
constexpr uint8_t FLASHCARD_PROGRESS_FILE_VERSION_LEGACY = 4;
constexpr char FLASHCARD_PROGRESS_FILE[] = "/.crosspoint/flashcards_global.bin";
constexpr size_t MAX_FLASHCARDS_TOTAL = 900;
constexpr size_t FLASHCARD_BATCH_SIZE = 20;
constexpr uint8_t CARD_TEXT_SIZE_SMALL = 0;
constexpr uint8_t CARD_TEXT_SIZE_MEDIUM = 1;
constexpr uint8_t CARD_TEXT_SIZE_LARGE = 2;

constexpr std::array<uint16_t, 3> SM2PP_LEARNING_STEPS = {1, 8, 48};
constexpr uint16_t SM2PP_MATURE_INTERVAL = 21;
constexpr uint16_t SM2PP_MAX_INTERVAL = 4096;
constexpr uint16_t SM2PP_MIN_EASE_X100 = 130;
constexpr uint16_t SM2PP_MAX_EASE_X100 = 300;

template <typename T>
T clampValue(const T value, const T minValue, const T maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    while (*s1 && *s2) {
      if (isdigit(*s1) && isdigit(*s2)) {
        while (*s1 == '0') {
          s1++;
        }
        while (*s2 == '0') {
          s2++;
        }

        int len1 = 0;
        int len2 = 0;
        while (isdigit(s1[len1])) {
          len1++;
        }
        while (isdigit(s2[len2])) {
          len2++;
        }

        if (len1 != len2) {
          return len1 < len2;
        }

        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) {
            return s1[i] < s2[i];
          }
        }

        s1 += len1;
        s2 += len2;
      } else {
        const char c1 = static_cast<char>(tolower(*s1));
        const char c2 = static_cast<char>(tolower(*s2));
        if (c1 != c2) {
          return c1 < c2;
        }
        s1++;
        s2++;
      }
    }

    return *s1 == '\0' && *s2 != '\0';
  });
}
}  // namespace

void FlashcardsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FlashcardsActivity*>(param);
  self->displayTaskLoop();
}

void FlashcardsActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  cards.clear();
  progressRecords.clear();
  activeBatch.clear();

  reviewStep = 0;
  nextBatchStartOffset = 0;

  currentCardIndex = -1;
  showingAnswer = false;
  cardTextSize = CARD_TEXT_SIZE_MEDIUM;

  screenMode = ScreenMode::START;
  statusMessage.clear();

  loadProgress();
  loadAllFlashcards();
  if (!cards.empty()) {
    restoreOrCreateBatch();
    selectNextCard(true);
    saveProgress();
  }

  updateRequired = true;

  xTaskCreate(&FlashcardsActivity::taskTrampoline, "FlashcardsActivityTask",
              6144,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FlashcardsActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  saveProgress();

  cards.clear();
  progressRecords.clear();
  activeBatch.clear();
}

void FlashcardsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    onGoHome();
    return;
  }

  if (screenMode == ScreenMode::START) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      adjustCardTextSize(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      adjustCardTextSize(1);
      return;
    }

    if (!cards.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      screenMode = ScreenMode::STUDY;
      if (currentCardIndex < 0 || currentCardIndex >= static_cast<int>(cards.size())) {
        selectNextCard(true);
      }
      updateRequired = true;
    }
    return;
  }

  if (cards.empty() || currentCardIndex < 0 || currentCardIndex >= static_cast<int>(cards.size())) {
    screenMode = ScreenMode::START;
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    showingAnswer = !showingAnswer;
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    rateCurrentCard(Sm2ppRating::GOOD);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    rateCurrentCard(Sm2ppRating::HARD);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    rateCurrentCard(Sm2ppRating::EASY);
    return;
  }
}

void FlashcardsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void FlashcardsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Flashcards");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (screenMode == ScreenMode::START || cards.empty() || currentCardIndex < 0 ||
      currentCardIndex >= static_cast<int>(cards.size())) {
    if (cards.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 30, "No flashcards loaded");
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 55, "Put .txt files into /flashcards");
      if (!statusMessage.empty()) {
        const std::string status =
            renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - metrics.contentSidePadding * 2);
        renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 80, status.c_str());
      }
      char textSizeLine[64];
      snprintf(textSizeLine, sizeof(textSizeLine), "Card text size: %s", getCardTextSizeLabel());
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 115, textSizeLine);
      const auto labels = mappedInput.mapLabels("Back", "", "-", "+");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    const int memorizedCards = countMemorizedCards();
    const int processedInBatch = countProcessedInBatch();
    const int batchSize = static_cast<int>(activeBatch.size());
    const int totalBatchCards = batchSize > 0 ? batchSize : 0;

    char line1[96];
    char line2[96];
    char line3[96];
    snprintf(line1, sizeof(line1), "Memorized cards: %d/%d", memorizedCards, static_cast<int>(cards.size()));
    snprintf(line2, sizeof(line2), "Active batch: %d/%d", processedInBatch, totalBatchCards);
    snprintf(line3, sizeof(line3), "Card text size: %s", getCardTextSizeLabel());

    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 30, line1);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 55, line2);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 80, line3);

    renderer.drawCenteredText(UI_12_FONT_ID, contentBottom - 30, "Press Confirm to Learn", true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels("Back", "Learn", "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto& currentCard = cards[currentCardIndex];
  const auto& progress = progressRecords[currentCard.progressIndex];

  const int batchPosition = batchPositionForCurrentCard();
  const int batchSize = std::max(1, static_cast<int>(activeBatch.size()));

  char cardCounter[96];
  snprintf(cardCounter, sizeof(cardCounter), "Card %d/%d (%d total)", batchPosition, batchSize,
           static_cast<int>(cards.size()));
  renderer.drawCenteredText(UI_10_FONT_ID, contentTop, cardCounter);

  const std::string memoryInfo = getMemorizationInfo(progress);
  renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 22, memoryInfo.c_str());

  const int cardX = metrics.contentSidePadding;
  const int cardY = contentTop + 50;
  const int cardWidth = pageWidth - metrics.contentSidePadding * 2;
  const int cardHeight = contentHeight - 100;
  renderer.drawRoundedRect(cardX, cardY, cardWidth, cardHeight, 2, 12, true);

  const char* sideLabel = showingAnswer ? "Translation" : "Prompt";
  renderer.drawCenteredText(UI_10_FONT_ID, cardY + 12, sideLabel, true, EpdFontFamily::BOLD);
  renderer.drawLine(cardX + 18, cardY + 36, cardX + cardWidth - 18, cardY + 36);

  const std::string& cardText = showingAnswer ? currentCard.answer : currentCard.prompt;
  const int textMaxWidth = cardWidth - 40;
  const int textAreaTop = cardY + 50;
  const int textAreaHeight = cardHeight - 80;
  const int cardTextFontId = getCardTextFontId();
  const int lineHeight = renderer.getLineHeight(cardTextFontId);
  const int maxLines = std::max(1, textAreaHeight / lineHeight);
  const auto wrapped = wrapCardText(renderer, cardText, cardTextFontId, textMaxWidth, maxLines);

  const int textBlockHeight = static_cast<int>(wrapped.size()) * lineHeight;
  int y = textAreaTop + std::max(0, (textAreaHeight - textBlockHeight) / 2);
  for (const auto& line : wrapped) {
    renderer.drawCenteredText(cardTextFontId, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lineHeight;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, contentBottom - 25, "Up/Down: Flip card");

  const auto labels = mappedInput.mapLabels("Back", "Hard", "Good", "Easy");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

bool FlashcardsActivity::loadProgress() {
  FsFile file;
  if (!Storage.openFileForRead("FCD", FLASHCARD_PROGRESS_FILE, file)) {
    return false;
  }

  uint8_t version = 0;
  serialization::readPod(file, version);
  if (version != FLASHCARD_PROGRESS_FILE_VERSION && version != FLASHCARD_PROGRESS_FILE_VERSION_LEGACY) {
    file.close();
    return false;
  }

  serialization::readPod(file, reviewStep);
  serialization::readPod(file, nextBatchStartOffset);

  uint16_t count = 0;
  serialization::readPod(file, count);
  if (count > 5000) {
    file.close();
    return false;
  }

  progressRecords.clear();
  progressRecords.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    FlashcardProgress progress;
    serialization::readPod(file, progress.key);

    serialization::readPod(file, progress.reviewCount);
    serialization::readPod(file, progress.hardCount);
    serialization::readPod(file, progress.goodCount);
    serialization::readPod(file, progress.easyCount);

    serialization::readPod(file, progress.sm2ppPhase);
    serialization::readPod(file, progress.sm2ppLearningStep);
    serialization::readPod(file, progress.sm2ppLapses);
    serialization::readPod(file, progress.sm2ppInterval);
    serialization::readPod(file, progress.sm2ppEaseX100);
    serialization::readPod(file, progress.sm2ppDueStep);

    if (progress.sm2ppPhase > static_cast<uint8_t>(Sm2ppPhase::REVIEW)) {
      progress.sm2ppPhase = static_cast<uint8_t>(Sm2ppPhase::LEARNING);
    }

    if (progress.sm2ppLearningStep >= SM2PP_LEARNING_STEPS.size()) {
      progress.sm2ppLearningStep = static_cast<uint8_t>(SM2PP_LEARNING_STEPS.size() - 1);
    }

    progress.sm2ppInterval = clampValue<uint16_t>(progress.sm2ppInterval, 0, SM2PP_MAX_INTERVAL);
    progress.sm2ppEaseX100 = clampValue<uint16_t>(progress.sm2ppEaseX100, SM2PP_MIN_EASE_X100, SM2PP_MAX_EASE_X100);

    progressRecords.push_back(progress);
  }

  uint8_t batchCount = 0;
  serialization::readPod(file, batchCount);
  if (batchCount > FLASHCARD_BATCH_SIZE) {
    file.close();
    activeBatch.clear();
    return false;
  }

  activeBatch.clear();
  activeBatch.reserve(batchCount);
  for (uint8_t i = 0; i < batchCount; i++) {
    BatchCard batchCard;
    serialization::readPod(file, batchCard.key);
    serialization::readPod(file, batchCard.processed);
    batchCard.processed = batchCard.processed == 0 ? 0 : 1;
    activeBatch.push_back(batchCard);
  }

  if (version >= FLASHCARD_PROGRESS_FILE_VERSION) {
    serialization::readPod(file, cardTextSize);
  } else {
    cardTextSize = CARD_TEXT_SIZE_MEDIUM;
  }
  cardTextSize = static_cast<uint8_t>(clampValue<int>(cardTextSize, CARD_TEXT_SIZE_SMALL, CARD_TEXT_SIZE_LARGE));

  file.close();
  return true;
}

bool FlashcardsActivity::saveProgress() const {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("FCD", FLASHCARD_PROGRESS_FILE, file)) {
    return false;
  }

  serialization::writePod(file, FLASHCARD_PROGRESS_FILE_VERSION);
  serialization::writePod(file, reviewStep);
  serialization::writePod(file, nextBatchStartOffset);

  const uint16_t count = static_cast<uint16_t>(progressRecords.size());
  serialization::writePod(file, count);
  for (const auto& progress : progressRecords) {
    serialization::writePod(file, progress.key);

    serialization::writePod(file, progress.reviewCount);
    serialization::writePod(file, progress.hardCount);
    serialization::writePod(file, progress.goodCount);
    serialization::writePod(file, progress.easyCount);

    serialization::writePod(file, progress.sm2ppPhase);
    serialization::writePod(file, progress.sm2ppLearningStep);
    serialization::writePod(file, progress.sm2ppLapses);
    serialization::writePod(file, progress.sm2ppInterval);
    serialization::writePod(file, progress.sm2ppEaseX100);
    serialization::writePod(file, progress.sm2ppDueStep);
  }

  const uint8_t batchCount = static_cast<uint8_t>(std::min(activeBatch.size(), FLASHCARD_BATCH_SIZE));
  serialization::writePod(file, batchCount);
  for (uint8_t i = 0; i < batchCount; i++) {
    serialization::writePod(file, activeBatch[i].key);
    serialization::writePod(file, activeBatch[i].processed);
  }
  serialization::writePod(file, cardTextSize);

  file.close();
  return true;
}

size_t FlashcardsActivity::findOrCreateProgressRecord(const uint32_t key) {
  for (size_t i = 0; i < progressRecords.size(); i++) {
    if (progressRecords[i].key == key) {
      return i;
    }
  }

  FlashcardProgress progress;
  progress.key = key;
  progressRecords.push_back(progress);
  return progressRecords.size() - 1;
}

int FlashcardsActivity::findCardIndexByKey(const uint32_t key) const {
  for (int i = 0; i < static_cast<int>(cards.size()); i++) {
    if (cards[i].key == key) {
      return i;
    }
  }
  return -1;
}

bool FlashcardsActivity::loadAllFlashcards() {
  cards.clear();

  const std::string folderPath = getFlashcardsFolderPath();
  auto root = Storage.open(folderPath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    statusMessage = "Folder /flashcards was not found";
    return false;
  }

  std::vector<std::string> txtFiles;
  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.') {
      file.close();
      continue;
    }

    if (!file.isDirectory()) {
      const std::string fileName(name);
      if (isTxtFile(fileName)) {
        txtFiles.push_back(fileName);
      }
    }

    file.close();
  }

  root.close();

  if (txtFiles.empty()) {
    statusMessage = "No .txt files found in /flashcards";
    return false;
  }

  sortFileList(txtFiles);

  int skippedLines = 0;
  int duplicateLines = 0;
  bool reachedLimit = false;
  int filesWithCards = 0;

  for (const auto& fileName : txtFiles) {
    std::string path = folderPath;
    if (!path.empty() && path.back() != '/') {
      path += "/";
    }
    path += fileName;

    const size_t countBefore = cards.size();
    int fileSkipped = 0;
    int fileDuplicates = 0;
    bool fileReachedLimit = false;
    if (parseFlashcardsFile(path, fileSkipped, fileDuplicates, fileReachedLimit)) {
      if (cards.size() > countBefore) {
        filesWithCards++;
      }
    }

    skippedLines += fileSkipped;
    duplicateLines += fileDuplicates;

    if (fileReachedLimit) {
      reachedLimit = true;
      break;
    }
  }

  if (cards.empty()) {
    statusMessage = "No valid cards in /flashcards/*.txt";
    return false;
  }

  statusMessage = "Loaded " + std::to_string(cards.size()) + " cards from " + std::to_string(filesWithCards) + " files";
  if (skippedLines > 0) {
    statusMessage += " | skipped " + std::to_string(skippedLines);
  }
  if (duplicateLines > 0) {
    statusMessage += " | dup " + std::to_string(duplicateLines);
  }
  if (reachedLimit) {
    statusMessage += " | limit reached";
  }

  return true;
}

bool FlashcardsActivity::parseFlashcardsFile(const std::string& path, int& skippedLines, int& duplicateLines,
                                             bool& reachedLimit) {
  FsFile file;
  if (!Storage.openFileForRead("FCD", path, file)) {
    return false;
  }

  reachedLimit = false;
  std::string line;
  while (readLine(file, line)) {
    const std::string trimmedLine = trim(line);
    if (trimmedLine.empty() || trimmedLine[0] == '#') {
      continue;
    }

    size_t splitPos = trimmedLine.find('\t');
    if (splitPos == std::string::npos) {
      splitPos = trimmedLine.rfind(',');
    }
    if (splitPos == std::string::npos) {
      skippedLines++;
      continue;
    }

    const std::string prompt = trim(trimmedLine.substr(0, splitPos));
    const std::string answer = trim(trimmedLine.substr(splitPos + 1));
    if (prompt.empty() || answer.empty()) {
      skippedLines++;
      continue;
    }

    const uint32_t key = hashCard(prompt, answer);
    const bool alreadyExists =
        std::any_of(cards.begin(), cards.end(), [key](const Flashcard& card) { return card.key == key; });
    if (alreadyExists) {
      duplicateLines++;
      continue;
    }

    if (cards.size() >= MAX_FLASHCARDS_TOTAL) {
      reachedLimit = true;
      break;
    }

    Flashcard card;
    card.key = key;
    card.progressIndex = findOrCreateProgressRecord(key);
    card.prompt = prompt;
    card.answer = answer;
    cards.push_back(card);
  }

  file.close();
  return true;
}

bool FlashcardsActivity::isTxtFile(const std::string& fileName) { return StringUtils::checkFileExtension(fileName, ".txt"); }

std::string FlashcardsActivity::getFlashcardsFolderPath() {
  return FlashcardsModel::findFlashcardsFolder([](const std::string& path) {
    FsFile dir = Storage.open(path.c_str());
    const bool isDirectory = dir && dir.isDirectory();
    if (dir) {
      dir.close();
    }
    return isDirectory;
  });
}

void FlashcardsActivity::restoreOrCreateBatch() {
  if (cards.empty()) {
    activeBatch.clear();
    currentCardIndex = -1;
    return;
  }

  const uint16_t totalCards = static_cast<uint16_t>(cards.size());
  nextBatchStartOffset = static_cast<uint16_t>(nextBatchStartOffset % totalCards);

  std::vector<BatchCard> restoredBatch;
  restoredBatch.reserve(activeBatch.size());

  for (const auto& batchCard : activeBatch) {
    if (findCardIndexByKey(batchCard.key) < 0) {
      continue;
    }

    const bool duplicate =
        std::any_of(restoredBatch.begin(), restoredBatch.end(),
                    [batchCard](const BatchCard& candidate) { return candidate.key == batchCard.key; });
    if (duplicate) {
      continue;
    }

    BatchCard normalized = batchCard;
    normalized.processed = normalized.processed == 0 ? 0 : 1;
    restoredBatch.push_back(normalized);

    if (restoredBatch.size() >= FLASHCARD_BATCH_SIZE) {
      break;
    }
  }

  activeBatch = std::move(restoredBatch);
  if (activeBatch.empty() || isActiveBatchComplete()) {
    createNextBatch();
  }
}

void FlashcardsActivity::createNextBatch() {
  activeBatch.clear();
  if (cards.empty()) {
    return;
  }

  const size_t totalCards = cards.size();
  const size_t batchSize = std::min(FLASHCARD_BATCH_SIZE, totalCards);
  const size_t start = nextBatchStartOffset % totalCards;

  activeBatch.reserve(batchSize);
  for (size_t i = 0; i < batchSize; i++) {
    const size_t index = (start + i) % totalCards;

    BatchCard batchCard;
    batchCard.key = cards[index].key;
    batchCard.processed = 0;
    activeBatch.push_back(batchCard);
  }

  nextBatchStartOffset = static_cast<uint16_t>((start + batchSize) % totalCards);
}

int FlashcardsActivity::countProcessedInBatch() const {
  return static_cast<int>(
      std::count_if(activeBatch.begin(), activeBatch.end(), [](const BatchCard& card) { return card.processed != 0; }));
}

bool FlashcardsActivity::isActiveBatchComplete() const {
  return !activeBatch.empty() && countProcessedInBatch() >= static_cast<int>(activeBatch.size());
}

int FlashcardsActivity::batchPositionForCurrentCard() const {
  if (currentCardIndex < 0 || currentCardIndex >= static_cast<int>(cards.size())) {
    return 0;
  }

  const uint32_t currentKey = cards[currentCardIndex].key;
  for (int i = 0; i < static_cast<int>(activeBatch.size()); i++) {
    if (activeBatch[i].key == currentKey) {
      return i + 1;
    }
  }

  return 0;
}

uint32_t FlashcardsActivity::getDueStep(const FlashcardProgress& progress) const { return progress.sm2ppDueStep; }

int FlashcardsActivity::findNextCardIndex(const bool includeFutureCards) const {
  if (cards.empty() || activeBatch.empty()) {
    return -1;
  }

  int bestIndex = -1;
  uint32_t bestDue = std::numeric_limits<uint32_t>::max();
  const int totalCards = static_cast<int>(cards.size());

  const auto ringDistance = [this, totalCards](const int index) {
    if (currentCardIndex < 0 || currentCardIndex >= totalCards) {
      return index;
    }

    int distance = index - currentCardIndex;
    if (distance <= 0) {
      distance += totalCards;
    }
    return distance;
  };

  for (const auto& batchCard : activeBatch) {
    if (batchCard.processed != 0) {
      continue;
    }

    const int cardIndex = findCardIndexByKey(batchCard.key);
    if (cardIndex < 0) {
      continue;
    }

    const auto& progress = progressRecords[cards[cardIndex].progressIndex];
    const uint32_t due = getDueStep(progress);

    if (!includeFutureCards && due > reviewStep) {
      continue;
    }

    if (bestIndex < 0 || due < bestDue || (due == bestDue && ringDistance(cardIndex) < ringDistance(bestIndex))) {
      bestIndex = cardIndex;
      bestDue = due;
    }
  }

  return bestIndex;
}

void FlashcardsActivity::selectNextCard(const bool includeFutureCards) {
  if (cards.empty()) {
    currentCardIndex = -1;
    showingAnswer = false;
    return;
  }

  if (activeBatch.empty()) {
    restoreOrCreateBatch();
  }

  if (isActiveBatchComplete()) {
    createNextBatch();
  }

  currentCardIndex = findNextCardIndex(includeFutureCards);
  showingAnswer = false;
}

void FlashcardsActivity::adjustCardTextSize(const int delta) {
  const int next = clampValue<int>(static_cast<int>(cardTextSize) + delta, CARD_TEXT_SIZE_SMALL, CARD_TEXT_SIZE_LARGE);
  if (next == static_cast<int>(cardTextSize)) {
    return;
  }

  cardTextSize = static_cast<uint8_t>(next);
  saveProgress();
  updateRequired = true;
}

int FlashcardsActivity::getCardTextFontId() const {
  if (cardTextSize <= CARD_TEXT_SIZE_SMALL) {
    return UI_10_FONT_ID;
  }
  if (cardTextSize >= CARD_TEXT_SIZE_LARGE) {
    return NOTOSANS_14_FONT_ID;
  }
  return UI_12_FONT_ID;
}

const char* FlashcardsActivity::getCardTextSizeLabel() const {
  if (cardTextSize <= CARD_TEXT_SIZE_SMALL) {
    return "Small";
  }
  if (cardTextSize >= CARD_TEXT_SIZE_LARGE) {
    return "Large";
  }
  return "Medium";
}

void FlashcardsActivity::rateCurrentCard(const Sm2ppRating rating) {
  if (currentCardIndex < 0 || currentCardIndex >= static_cast<int>(cards.size())) {
    return;
  }

  const uint32_t ratedCardKey = cards[currentCardIndex].key;
  auto& progress = progressRecords[cards[currentCardIndex].progressIndex];

  reviewStep++;

  if (progress.reviewCount < std::numeric_limits<uint16_t>::max()) {
    progress.reviewCount++;
  }

  switch (rating) {
    case Sm2ppRating::HARD:
      if (progress.hardCount < std::numeric_limits<uint16_t>::max()) {
        progress.hardCount++;
      }
      break;
    case Sm2ppRating::GOOD:
      if (progress.goodCount < std::numeric_limits<uint16_t>::max()) {
        progress.goodCount++;
      }
      break;
    case Sm2ppRating::EASY:
      if (progress.easyCount < std::numeric_limits<uint16_t>::max()) {
        progress.easyCount++;
      }
      break;
  }

  applySm2pp(progress, rating);

  for (auto& batchCard : activeBatch) {
    if (batchCard.key == ratedCardKey) {
      batchCard.processed = (rating == Sm2ppRating::EASY) ? 1 : 0;
      break;
    }
  }

  if (isActiveBatchComplete()) {
    createNextBatch();
  }

  selectNextCard(false);
  if (currentCardIndex < 0) {
    selectNextCard(true);
  }

  saveProgress();
  updateRequired = true;
}

void FlashcardsActivity::applySm2pp(FlashcardProgress& progress, const Sm2ppRating rating) {
  const auto phase = static_cast<Sm2ppPhase>(progress.sm2ppPhase);

  if (phase == Sm2ppPhase::LEARNING || phase == Sm2ppPhase::RELEARNING) {
    const bool relearning = phase == Sm2ppPhase::RELEARNING;

    if (rating == Sm2ppRating::HARD) {
      if (progress.sm2ppLearningStep > 0) {
        progress.sm2ppLearningStep--;
      }
      progress.sm2ppEaseX100 = static_cast<uint16_t>(
          clampValue(static_cast<int>(progress.sm2ppEaseX100) - 15, static_cast<int>(SM2PP_MIN_EASE_X100),
                     static_cast<int>(SM2PP_MAX_EASE_X100)));
      progress.sm2ppDueStep = reviewStep + SM2PP_LEARNING_STEPS[progress.sm2ppLearningStep];
      return;
    }

    if (rating == Sm2ppRating::GOOD) {
      if (progress.sm2ppLearningStep + 1 < SM2PP_LEARNING_STEPS.size()) {
        progress.sm2ppLearningStep++;
        progress.sm2ppDueStep = reviewStep + SM2PP_LEARNING_STEPS[progress.sm2ppLearningStep];
        return;
      }

      progress.sm2ppPhase = static_cast<uint8_t>(Sm2ppPhase::REVIEW);
      progress.sm2ppLearningStep = 0;

      if (progress.sm2ppInterval == 0) {
        progress.sm2ppInterval = relearning ? 2 : 1;
      }
      progress.sm2ppInterval = applyIntervalFuzz(progress.sm2ppInterval, progress.key);
      progress.sm2ppDueStep = reviewStep + std::max<uint16_t>(1, progress.sm2ppInterval);
      return;
    }

    progress.sm2ppPhase = static_cast<uint8_t>(Sm2ppPhase::REVIEW);
    progress.sm2ppLearningStep = 0;
    progress.sm2ppEaseX100 = static_cast<uint16_t>(
        clampValue(static_cast<int>(progress.sm2ppEaseX100) + 15, static_cast<int>(SM2PP_MIN_EASE_X100),
                   static_cast<int>(SM2PP_MAX_EASE_X100)));

    if (relearning) {
      const uint16_t base = std::max<uint16_t>(3, static_cast<uint16_t>(std::max<uint16_t>(1, progress.sm2ppInterval) * 3 / 2));
      progress.sm2ppInterval = applyIntervalFuzz(base, progress.key);
    } else {
      progress.sm2ppInterval = applyIntervalFuzz(4, progress.key);
    }

    progress.sm2ppDueStep = reviewStep + std::max<uint16_t>(1, progress.sm2ppInterval);
    return;
  }

  uint16_t interval = std::max<uint16_t>(1, progress.sm2ppInterval);

  if (rating == Sm2ppRating::HARD) {
    if (progress.sm2ppLapses < std::numeric_limits<uint8_t>::max()) {
      progress.sm2ppLapses++;
    }

    progress.sm2ppPhase = static_cast<uint8_t>(Sm2ppPhase::RELEARNING);
    progress.sm2ppLearningStep = 0;
    progress.sm2ppInterval = std::max<uint16_t>(1, static_cast<uint16_t>(interval / 2));
    progress.sm2ppEaseX100 = static_cast<uint16_t>(
        clampValue(static_cast<int>(progress.sm2ppEaseX100) - 20, static_cast<int>(SM2PP_MIN_EASE_X100),
                   static_cast<int>(SM2PP_MAX_EASE_X100)));
    progress.sm2ppDueStep = reviewStep + SM2PP_LEARNING_STEPS[0];
    return;
  }

  if (rating == Sm2ppRating::GOOD) {
    const float ease = static_cast<float>(progress.sm2ppEaseX100) / 100.0f;
    const int base = std::max<int>(interval + 1, static_cast<int>(std::lround(interval * ease)));
    progress.sm2ppInterval = applyIntervalFuzz(
        static_cast<uint16_t>(clampValue(base, 1, static_cast<int>(SM2PP_MAX_INTERVAL))), progress.key);
    progress.sm2ppDueStep = reviewStep + std::max<uint16_t>(1, progress.sm2ppInterval);
    return;
  }

  progress.sm2ppEaseX100 = static_cast<uint16_t>(
      clampValue(static_cast<int>(progress.sm2ppEaseX100) + 15, static_cast<int>(SM2PP_MIN_EASE_X100),
                 static_cast<int>(SM2PP_MAX_EASE_X100)));
  const float ease = static_cast<float>(progress.sm2ppEaseX100) / 100.0f;
  const int base = std::max<int>(interval + 2, static_cast<int>(std::lround(interval * ease * 1.3f)));
  progress.sm2ppInterval =
      applyIntervalFuzz(static_cast<uint16_t>(clampValue(base, 1, static_cast<int>(SM2PP_MAX_INTERVAL))), progress.key);
  progress.sm2ppDueStep = reviewStep + std::max<uint16_t>(1, progress.sm2ppInterval);
}

uint16_t FlashcardsActivity::applyIntervalFuzz(const uint16_t baseInterval, const uint32_t key) const {
  if (baseInterval <= 2) {
    return baseInterval;
  }

  const int range = std::max(1, static_cast<int>(baseInterval / 20));
  const uint32_t mix = key ^ (reviewStep * 2654435761u);
  const int delta = static_cast<int>(mix % static_cast<uint32_t>(range * 2 + 1)) - range;

  return static_cast<uint16_t>(
      clampValue(static_cast<int>(baseInterval) + delta, 1, static_cast<int>(SM2PP_MAX_INTERVAL)));
}

int FlashcardsActivity::countMemorizedCards() const {
  return static_cast<int>(
      std::count_if(cards.begin(), cards.end(), [this](const Flashcard& card) {
        return isCardMemorized(progressRecords[card.progressIndex]);
      }));
}

bool FlashcardsActivity::isCardMemorized(const FlashcardProgress& progress) const {
  const auto phase = static_cast<Sm2ppPhase>(progress.sm2ppPhase);
  return phase == Sm2ppPhase::REVIEW && progress.sm2ppInterval > 0;
}

std::string FlashcardsActivity::getMemorizationInfo(const FlashcardProgress& progress) const {
  char line[128];

  switch (static_cast<Sm2ppPhase>(progress.sm2ppPhase)) {
    case Sm2ppPhase::LEARNING:
      snprintf(line, sizeof(line), "Memory: Learning %u/%u", static_cast<unsigned>(progress.sm2ppLearningStep + 1),
               static_cast<unsigned>(SM2PP_LEARNING_STEPS.size()));
      break;

    case Sm2ppPhase::RELEARNING:
      snprintf(line, sizeof(line), "Memory: Relearning %u/%u", static_cast<unsigned>(progress.sm2ppLearningStep + 1),
               static_cast<unsigned>(SM2PP_LEARNING_STEPS.size()));
      break;

    case Sm2ppPhase::REVIEW:
    default: {
      const char* stateLabel = progress.sm2ppInterval >= SM2PP_MATURE_INTERVAL ? "Mature" : "Young";
      const int easeWhole = progress.sm2ppEaseX100 / 100;
      const int easeFraction = progress.sm2ppEaseX100 % 100;
      snprintf(line, sizeof(line), "Memory: %s | EF %d.%02d | Ivl %u", stateLabel, easeWhole, easeFraction,
               progress.sm2ppInterval);
      break;
    }
  }

  return line;
}

std::vector<std::string> FlashcardsActivity::wrapCardText(const GfxRenderer& renderer, const std::string& text,
                                                          const int fontId, const int maxWidth, const int maxLines) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const char c : text) {
    if (c == '\r' || c == '\n' || c == '\t') {
      normalized.push_back(' ');
    } else {
      normalized.push_back(c);
    }
  }

  std::vector<std::string> words;
  words.reserve(24);
  size_t pos = 0;
  while (pos < normalized.size()) {
    while (pos < normalized.size() && normalized[pos] == ' ') {
      pos++;
    }
    if (pos >= normalized.size()) {
      break;
    }
    const size_t start = pos;
    while (pos < normalized.size() && normalized[pos] != ' ') {
      pos++;
    }
    words.emplace_back(normalized.substr(start, pos - start));
  }

  if (words.empty()) {
    return {""};
  }

  std::vector<std::string> lines;
  lines.reserve(maxLines);
  size_t wordIndex = 0;
  while (wordIndex < words.size() && static_cast<int>(lines.size()) < maxLines) {
    std::string line = words[wordIndex++];
    if (renderer.getTextWidth(fontId, line.c_str()) > maxWidth) {
      lines.push_back(renderer.truncatedText(fontId, line.c_str(), maxWidth));
      continue;
    }

    while (wordIndex < words.size()) {
      const std::string candidate = line + " " + words[wordIndex];
      if (renderer.getTextWidth(fontId, candidate.c_str()) > maxWidth) {
        break;
      }
      line = candidate;
      wordIndex++;
    }

    lines.push_back(line);
  }

  if (wordIndex < words.size() && !lines.empty()) {
    std::string clipped = lines.back();
    while (!clipped.empty() && renderer.getTextWidth(fontId, (clipped + "...").c_str()) > maxWidth) {
      utf8RemoveLastChar(clipped);
    }
    lines.back() = clipped.empty() ? "..." : clipped + "...";
  }

  return lines;
}

std::string FlashcardsActivity::trim(const std::string& value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    start++;
  }

  if (start >= value.size()) {
    return "";
  }

  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end--;
  }

  return value.substr(start, end - start);
}

bool FlashcardsActivity::readLine(FsFile& file, std::string& outLine) {
  outLine.clear();
  while (true) {
    const int raw = file.read();
    if (raw < 0) {
      return !outLine.empty();
    }

    const char c = static_cast<char>(raw);
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      return true;
    }
    outLine.push_back(c);
  }
}

uint32_t FlashcardsActivity::hashCard(const std::string& prompt, const std::string& answer) {
  uint32_t hash = 2166136261u;
  const auto hashByte = [&hash](const unsigned char c) {
    hash ^= c;
    hash *= 16777619u;
  };

  for (const unsigned char c : prompt) {
    hashByte(c);
  }
  hashByte('\t');
  for (const unsigned char c : answer) {
    hashByte(c);
  }

  return hash;
}
