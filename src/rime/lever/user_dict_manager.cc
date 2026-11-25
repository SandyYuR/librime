//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-03-23 GONG Chen <chen.sst@gmail.com>
//
#include <fstream>
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <boost/scope_exit.hpp>
#include <rime/common.h>
#include <rime/deployer.h>
#include <rime/algo/utilities.h>
#include <rime/dict/db_utils.h>
#include <rime/dict/table_db.h>
#include <rime/dict/user_db.h>
#include <rime/lever/user_dict_manager.h>

namespace fs = std::filesystem;

namespace rime {

namespace {
// Use the database implementation's removal operation. LevelDB acquires its
// lock before deleting database files and leaves unrelated files untouched.
bool CleanRestoreScratch(Db* db, const char* stage) noexcept {
  try {
    if (db->loaded() && !db->Close()) {
      LOG(ERROR) << "failed to close restore scratch " << db->file_path()
                 << " (" << stage << ").";
      return false;
    }
    if (!db->Exists())
      return true;
    if (!db->Remove()) {
      LOG(ERROR) << "failed to remove restore scratch " << db->file_path()
                 << " (" << stage << ").";
      return false;
    }
    if (db->Exists()) {
      // DestroyDB can successfully remove the database while retaining the
      // directory and unrelated files. Do not recursively delete those files.
      LOG(WARNING) << "restore scratch path contains files not owned by the "
                      "database after cleanup: "
                   << db->file_path() << " (" << stage
                   << "); preserving those files.";
    }
    return true;
  } catch (const std::exception& ex) {
    LOG(ERROR) << "restore scratch cleanup failed (" << stage
               << "): " << ex.what();
  } catch (...) {
    LOG(ERROR) << "restore scratch cleanup failed (" << stage << ").";
  }
  return false;
}
}  // namespace

UserDictManager::UserDictManager(Deployer* deployer)
    : deployer_(deployer), user_db_component_(UserDb::Require("userdb")) {
  if (deployer) {
    path_ = deployer->user_data_dir;
  }
}

void UserDictManager::GetUserDictList(UserDictList* user_dict_list,
                                      UserDb::Component* component) {
  if (!user_dict_list)
    return;
  if (!component) {
    component = user_db_component_;
  }
  user_dict_list->clear();
  if (!fs::exists(path_) || !fs::is_directory(path_)) {
    LOG(INFO) << "directory '" << path_ << "' does not exist.";
    return;
  }
  for (fs::directory_iterator it(path_), end; it != end; ++it) {
    string name = path(it->path().filename()).to_utf8_string();
    if (boost::ends_with(name, component->extension())) {
      boost::erase_last(name, component->extension());
      if (name == ".temp")  // Reserved for Restore().
        continue;
      user_dict_list->push_back(name);
    }
  }
}

bool UserDictManager::Backup(const string& dict_name) {
  the<Db> db(user_db_component_->Create(dict_name));
  if (!db->OpenReadOnly())
    return false;
  if (UserDbHelper(db).GetUserId() != deployer_->user_id) {
    LOG(INFO) << "user id not match; recreating metadata in " << dict_name;
    if (!db->Close() || !db->Open() || !db->CreateMetadata()) {
      LOG(ERROR) << "failed to recreate metadata in " << dict_name;
      return false;
    }
  }
  const path& dir(deployer_->user_data_sync_dir());
  if (!fs::exists(dir)) {
    if (!fs::create_directories(dir)) {
      LOG(ERROR) << "error creating directory '" << dir << "'.";
      return false;
    }
  }
  string snapshot_file = dict_name + UserDb::snapshot_extension();
  return db->Backup(dir / snapshot_file);
}

bool UserDictManager::Restore(const path& snapshot_file) {
  the<Db> temp(user_db_component_->Create(".temp"));
  if (!CleanRestoreScratch(temp.get(), "before restore"))
    return false;

  // Open() may create files before reporting a failure. Arm cleanup before
  // opening; failed removal must never be followed by reopening old contents.
  bool cleanup_done = false;
  bool cleanup_succeeded = false;
  auto cleanup = [&]() noexcept {
    if (!cleanup_done) {
      cleanup_done = true;
      cleanup_succeeded = CleanRestoreScratch(temp.get(), "after restore");
    }
    return cleanup_succeeded;
  };
  BOOST_SCOPE_EXIT((&cleanup)) {
    cleanup();
  }
  BOOST_SCOPE_EXIT_END

  const bool restored = [&]() {
    if (!temp->Open())
      return false;
    {
      // Defend against a backend reporting successful removal without having
      // cleared its contents. Importing a snapshot only updates existing keys.
      auto records = temp->QueryAll();
      if (UserDbHelper(temp).GetDbName() != ".temp" || !records ||
          !records->exhausted()) {
        LOG(ERROR) << "restore scratch is not empty after cleanup: "
                   << temp->file_path();
        return false;
      }
    }
    if (!temp->Restore(snapshot_file))
      return false;
    if (!UserDbHelper(temp).IsUserDb())
      return false;
    string db_name = UserDbHelper(temp).GetDbName();
    if (db_name.empty())
      return false;
    the<Db> dest(user_db_component_->Create(db_name));
    if (!dest->Open())
      return false;
    BOOST_SCOPE_EXIT((&dest)) {
      dest->Close();
    }
    BOOST_SCOPE_EXIT_END
    LOG(INFO) << "merging '" << snapshot_file << "' from "
              << UserDbHelper(temp).GetUserId() << " into userdb '" << db_name
              << "'...";
    DbSource source(temp.get());
    UserDbMerger merger(dest.get());
    source >> merger;
    return true;
  }();
  // Close all merge cursors before removing the scratch and include cleanup
  // failure in this operation's result. The scope guard also covers exceptions.
  const bool cleaned = cleanup();
  return restored && cleaned;
}

int UserDictManager::Export(const string& dict_name, const path& text_file) {
  the<Db> db(user_db_component_->Create(dict_name));
  if (!db->OpenReadOnly())
    return -1;
  BOOST_SCOPE_EXIT((&db)) {
    db->Close();
  }
  BOOST_SCOPE_EXIT_END
  if (!UserDbHelper(db).IsUserDb())
    return -1;
  TsvWriter writer(text_file, TableDb::format.formatter);
  writer.file_description = "Rime user dictionary export";
  DbSource source(db.get());
  int num_entries = 0;
  try {
    num_entries = writer << source;
  } catch (std::exception& ex) {
    LOG(ERROR) << ex.what();
    return -1;
  }
  DLOG(INFO) << num_entries << " entries exported.";
  return num_entries;
}

int UserDictManager::Import(const string& dict_name, const path& text_file) {
  the<Db> db(user_db_component_->Create(dict_name));
  if (!db->Open())
    return -1;
  BOOST_SCOPE_EXIT((&db)) {
    db->Close();
  }
  BOOST_SCOPE_EXIT_END
  if (!UserDbHelper(db).IsUserDb())
    return -1;
  TsvReader reader(text_file, TableDb::format.parser);
  UserDbImporter importer(db.get());
  int num_entries = 0;
  try {
    num_entries = reader >> importer;
  } catch (std::exception& ex) {
    LOG(ERROR) << ex.what();
    return -1;
  }
  DLOG(INFO) << num_entries << " entries imported.";
  return num_entries;
}

bool UserDictManager::UpgradeUserDict(const string& dict_name) {
  UserDb::Component* legacy_component = UserDb::Require("legacy_userdb");
  if (!legacy_component)
    return true;
  the<Db> legacy_db(legacy_component->Create(dict_name));
  if (!legacy_db->Exists())
    return true;
  if (!legacy_db->OpenReadOnly() || !UserDbHelper(legacy_db).IsUserDb())
    return false;
  LOG(INFO) << "upgrading user dict '" << dict_name << "'.";
  path trash = deployer_->user_data_dir / "trash";
  if (!fs::exists(trash)) {
    std::error_code ec;
    if (!fs::create_directories(trash, ec)) {
      LOG(ERROR) << "error creating directory '" << trash << "'.";
      return false;
    }
  }
  string snapshot_file = dict_name + UserDb::snapshot_extension();
  path snapshot_path = trash / snapshot_file;
  return legacy_db->Backup(snapshot_path) && legacy_db->Close() &&
         legacy_db->Remove() && Restore(snapshot_path);
}

bool UserDictManager::Synchronize(const string& dict_name) {
  LOG(INFO) << "synchronize user dict '" << dict_name << "'.";
  bool success = true;
  path sync_dir(deployer_->sync_dir);
  if (!fs::exists(sync_dir)) {
    std::error_code ec;
    if (!fs::create_directories(sync_dir, ec)) {
      LOG(ERROR) << "error creating directory '" << sync_dir << "'.";
      return false;
    }
  }
  // *.userdb.txt
  string snapshot_file = dict_name + UserDb::snapshot_extension();
  for (fs::directory_iterator it(sync_dir), end; it != end; ++it) {
    if (!fs::is_directory(it->path()))
      continue;
    path file_path = path(it->path()) / snapshot_file;
    if (fs::exists(file_path)) {
      LOG(INFO) << "merging snapshot file: " << file_path;
      if (!Restore(file_path)) {
        LOG(ERROR) << "failed to merge snapshot file: " << file_path;
        success = false;
      }
    }
  }
  if (!Backup(dict_name)) {
    LOG(ERROR) << "error backing up user dict '" << dict_name << "'.";
    success = false;
  }
  return success;
}

bool UserDictManager::SynchronizeAll() {
  // Recover an interrupted restore even when this sync has no snapshots to
  // merge. Discovery itself remains read-only and continues to skip .temp.
  the<Db> temp(user_db_component_->Create(".temp"));
  if (!CleanRestoreScratch(temp.get(), "before sync"))
    return false;
  UserDictList user_dicts;
  GetUserDictList(&user_dicts);
  LOG(INFO) << "synchronizing " << user_dicts.size() << " user dicts.";
  int failure = 0;
  for (const string& dict_name : user_dicts) {
    if (!Synchronize(dict_name))
      ++failure;
  }
  if (failure) {
    LOG(ERROR) << "failed synchronizing " << failure << "/" << user_dicts.size()
               << " user dicts.";
  }
  return !failure;
}

}  // namespace rime
