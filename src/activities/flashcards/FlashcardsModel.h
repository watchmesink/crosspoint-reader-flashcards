#pragma once

#include <functional>
#include <string>
#include <vector>

namespace FlashcardsModel {

const std::vector<std::string>& folderCandidates();
std::string findFlashcardsFolder(const std::function<bool(const std::string&)>& isDirectory);

}  // namespace FlashcardsModel
