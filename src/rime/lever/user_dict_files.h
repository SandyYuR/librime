// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_USER_DICT_FILES_H_
#define RIME_USER_DICT_FILES_H_

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rime {
// Shared by sync, backup and the dictionary management API. No removal happens
// while listing: .temp is Restore's reserved scratch name, not a user
// dictionary.
inline std::vector<std::string> ListUserDictNames(
    const std::filesystem::path& directory,
    const std::string& extension) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    std::string name = entry.path().filename().u8string();
    if (name.size() < extension.size() ||
        name.compare(name.size() - extension.size(), extension.size(),
                     extension))
      continue;
    name.resize(name.size() - extension.size());
    if (name != ".temp")
      names.push_back(std::move(name));
  }
  return names;
}
}  // namespace rime
#endif
