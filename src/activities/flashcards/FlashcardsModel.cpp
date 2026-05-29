#include "FlashcardsModel.h"

namespace FlashcardsModel {

const std::vector<std::string>& folderCandidates() {
  static const std::vector<std::string> candidates = {
      "/flashcards", "/Flashcards", "/FLASHCARDS", "/~/flashcards", "/~/Flashcards", "/~/FLASHCARDS",
  };
  return candidates;
}

std::string findFlashcardsFolder(const std::function<bool(const std::string&)>& isDirectory) {
  for (const auto& candidate : folderCandidates()) {
    if (isDirectory(candidate)) {
      return candidate;
    }
  }
  return folderCandidates().front();
}

}  // namespace FlashcardsModel
