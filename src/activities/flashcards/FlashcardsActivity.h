#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class FlashcardsActivity final : public Activity {
  enum class ScreenMode : uint8_t { DECK_SELECT = 0, START = 1, STUDY = 2 };
  enum class DeckId : uint8_t { GERMAN = 0, UKRAINIAN = 1, ENGLISH = 2 };
  enum class Sm2ppPhase : uint8_t { LEARNING = 0, RELEARNING = 1, REVIEW = 2 };
  enum class Sm2ppRating : uint8_t { HARD = 0, GOOD = 1, EASY = 2 };
  enum class CardTextSize : uint8_t { SMALL = 0, MEDIUM = 1, LARGE = 2 };

  struct DeckDefinition {
    DeckId id;
    const char* label;
    const char* folderName;
    const char* progressFile;
  };

  struct Flashcard {
    uint32_t key = 0;
    size_t progressIndex = 0;
    std::string prompt;
    std::string answer;
  };

  struct FlashcardProgress {
    uint32_t key = 0;

    uint16_t reviewCount = 0;
    uint16_t hardCount = 0;
    uint16_t goodCount = 0;
    uint16_t easyCount = 0;

    uint8_t sm2ppPhase = static_cast<uint8_t>(Sm2ppPhase::LEARNING);
    uint8_t sm2ppLearningStep = 0;
    uint8_t sm2ppLapses = 0;
    uint16_t sm2ppInterval = 0;
    uint16_t sm2ppEaseX100 = 250;
    uint32_t sm2ppDueStep = 0;
  };

  struct BatchCard {
    uint32_t key = 0;
    uint8_t processed = 0;
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  bool updateRequired = false;

  ScreenMode screenMode = ScreenMode::DECK_SELECT;
  uint8_t deckSelectionIndex = 0;
  DeckId activeDeck = DeckId::GERMAN;
  bool deckLoaded = false;

  std::vector<Flashcard> cards;
  std::vector<FlashcardProgress> progressRecords;
  std::vector<BatchCard> activeBatch;

  uint32_t reviewStep = 0;
  uint16_t nextBatchStartOffset = 0;
  uint16_t studyStreakDays = 0;
  int32_t lastStudyUnixDay = -1;

  int currentCardIndex = -1;
  bool showingAnswer = false;
  uint8_t cardTextSize = static_cast<uint8_t>(CardTextSize::LARGE);

  std::string statusMessage;

  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  void resetDeckState();
  bool loadDeck(DeckId deck);
  void returnToDeckSelection();
  bool loadProgress();
  bool loadProgressFromPath(const char* path);
  bool saveProgress() const;
  bool saveProgressToPath(const char* path) const;
  size_t findOrCreateProgressRecord(uint32_t key);
  int findCardIndexByKey(uint32_t key) const;

  bool loadAllFlashcards();
  bool parseFlashcardsFile(const std::string& path, int& skippedLines, int& duplicateLines, bool& reachedLimit);
  static bool isTxtFile(const std::string& fileName);
  static const DeckDefinition& getDeckDefinition(DeckId deck);
  const DeckDefinition& getSelectedDeckDefinition() const;
  static std::string getFlashcardsRootPath();
  static std::string getDeckFolderPath(DeckId deck);
  static std::string buildUniqueDeckFilePath(const std::string& folderPath, const std::string& fileName);
  static void migrateLegacyFlashcards();

  void restoreOrCreateBatch();
  void createNextBatch();
  int countProcessedInBatch() const;
  bool isActiveBatchComplete() const;
  int batchPositionForCurrentCard() const;

  uint32_t getDueStep(const FlashcardProgress& progress) const;
  int findNextCardIndex(bool includeFutureCards) const;
  void selectNextCard(bool includeFutureCards);
  int getCardTextFontId() const;
  void updateStudyStreak();
  static int32_t getCurrentUnixDay();

  void rateCurrentCard(Sm2ppRating rating);
  void applySm2pp(FlashcardProgress& progress, Sm2ppRating rating);
  uint16_t applyIntervalFuzz(uint16_t baseInterval, uint32_t key) const;

  int countMemorizedCards() const;
  bool isCardMemorized(const FlashcardProgress& progress) const;
  std::string getMemorizationInfo(const FlashcardProgress& progress) const;

  static std::vector<std::string> wrapCardText(const GfxRenderer& renderer, const std::string& text, int fontId,
                                               int maxWidth, int maxLines);
  static std::string trim(const std::string& value);
  static bool readLine(FsFile& file, std::string& outLine);
  static uint32_t hashCard(const std::string& prompt, const std::string& answer);

 public:
  explicit FlashcardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void()>& onGoHome)
      : Activity("Flashcards", renderer, mappedInput), onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
};
