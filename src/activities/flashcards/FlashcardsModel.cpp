#include "FlashcardsModel.h"

#include <cctype>

namespace FlashcardsModel {

const std::vector<std::string>& folderCandidates() {
  static const std::vector<std::string> candidates = {
      "/~/flashcards", "/~/Flashcards", "/~/FLASHCARDS", "~/flashcards", "~/Flashcards", "~/FLASHCARDS",
      "/flashcards",   "/Flashcards",   "/FLASHCARDS",
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

bool isTxtFile(const std::string& fileName) {
  constexpr const char* extension = ".txt";
  constexpr size_t extensionLength = 4;
  if (fileName.length() < extensionLength) {
    return false;
  }

  const size_t offset = fileName.length() - extensionLength;
  for (size_t i = 0; i < extensionLength; i++) {
    if (static_cast<char>(std::tolower(fileName[offset + i])) != extension[i]) {
      return false;
    }
  }
  return true;
}

std::string entryFilePath(const std::string& folderPath, const std::string& entryName) {
  if (entryName.empty()) {
    return folderPath;
  }

  if (entryName.front() == '/') {
    return entryName;
  }

  std::string path = folderPath;
  if (!path.empty() && path.back() != '/') {
    path += "/";
  }
  path += entryName;
  return path;
}

}  // namespace FlashcardsModel
