// Copyright RIME Developers
// Distributed under the BSD License
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <rime/deployer.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_db.h>
#include <rime/lever/user_dict_manager.h>

namespace fs = std::filesystem;
using namespace rime;

namespace {
// The production component uses the global service directory. Keep these real
// LevelDB instances in the fixture instead, without changing global settings.
class RootedUserDbComponent : public UserDb::Component {
 public:
  explicit RootedUserDbComponent(const fs::path& root) : root_(root) {}

  string extension() const override { return ".userdb"; }

  Db* Create(const string& name) override {
    return new UserDbWrapper<LevelDb>(path(root_ / (name + extension())), name);
  }

 private:
  fs::path root_;
};

class RootedUserDictManager : public UserDictManager {
 public:
  RootedUserDictManager(Deployer* deployer, UserDb::Component* component)
      : UserDictManager(deployer) {
    user_db_component_ = component;
  }
};

class UserDictSyncTest : public ::testing::Test {
 protected:
  const fs::path root_ =
      fs::temp_directory_path() /
      boost::uuids::to_string(boost::uuids::random_generator()());
  RootedUserDbComponent component_{root_};
  Deployer deployer_;
  bool created_ = false;

  void SetUp() override {
    created_ = fs::create_directory(root_);
    ASSERT_TRUE(created_);
    deployer_.user_data_dir = path(root_);
    deployer_.sync_dir = path(root_ / "sync");
    deployer_.user_id = "this-device";
  }

  void TearDown() override {
    if (created_) {
      std::error_code ec;
      fs::remove_all(root_, ec);
      EXPECT_FALSE(ec);
    }
  }

  void CreateDictionary(const string& name, const string& key) {
    the<Db> db(component_.Create(name));
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->MetaUpdate("/user_id", deployer_.user_id));
    ASSERT_TRUE(db->Update(key, "c=1 d=1 t=1"));
    ASSERT_TRUE(db->Close());
  }

  void ExpectRecord(const string& name, const string& key) {
    the<Db> db(component_.Create(name));
    ASSERT_TRUE(db->OpenReadOnly());
    string value;
    EXPECT_TRUE(db->Fetch(key, &value));
    EXPECT_GT(UserDbValue(value).commits, 0);
    EXPECT_TRUE(db->Close());
  }
};
}  // namespace

TEST(RimeUserDictManagerTest, ScratchDatabaseIsNotListedAsAUserDictionary) {
  const fs::path root =
      fs::temp_directory_path() /
      boost::uuids::to_string(boost::uuids::random_generator()());
  ASSERT_TRUE(fs::create_directory(root));
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{root};
  auto* component = UserDb::Require("userdb");
  ASSERT_NE(nullptr, component);
  const auto extension = component->extension();
  for (const auto* name :
       {".temp", ".personal", "luna", "temp", "luna.temp", "词典"})
    fs::create_directory(root / fs::u8path(std::string(name) + extension));
  std::ofstream(root / (".temp" + extension) / "keep")
      << "leftover scratch data";
  Deployer deployer;
  deployer.user_data_dir = path(root);
  UserDictManager manager(&deployer);
  UserDictList names;
  manager.GetUserDictList(&names);
  std::sort(names.begin(), names.end());
  EXPECT_EQ((UserDictList{".personal", "luna", "luna.temp", "temp", "词典"}),
            names);
  EXPECT_TRUE(fs::exists(root / (".temp" + extension) / "keep"));

  // Exercise the actual sync entry with only the interrupted-restore residue.
  // Previously it tried opening .temp and returned failure.
  const auto scratch_only = root / "scratch-only";
  fs::create_directories(scratch_only / (".temp" + extension));
  Deployer scratch_deployer;
  scratch_deployer.user_data_dir = path(scratch_only);
  UserDictManager scratch_manager(&scratch_deployer);
  EXPECT_TRUE(scratch_manager.SynchronizeAll());
  EXPECT_TRUE(fs::exists(scratch_only / (".temp" + extension)));
}

TEST_F(UserDictSyncTest, RealUserDictionarySyncsAndMergesWithScratchResidue) {
  CreateDictionary("luna", "local \tLocal word");
  const auto residue = root_ / ".temp.userdb";
  ASSERT_TRUE(fs::create_directory(residue));
  std::ofstream(residue / "keep") << "do not delete during enumeration";
  RootedUserDictManager manager(&deployer_, &component_);

  ASSERT_TRUE(manager.SynchronizeAll());
  ExpectRecord("luna", "local \tLocal word");
  const auto snapshot = root_ / "sync" / "this-device" / "luna.userdb.txt";
  ASSERT_TRUE(fs::is_regular_file(snapshot));
  EXPECT_FALSE(fs::exists(root_ / "sync" / "this-device" / ".temp.userdb.txt"));
  EXPECT_TRUE(fs::exists(residue / "keep"));

  const auto peer = root_ / "sync" / "peer";
  ASSERT_TRUE(fs::create_directory(peer));
  std::ofstream(peer / "luna.userdb.txt")
      << "# Rime user dictionary\n"
         "#@/db_name\tluna\n"
         "#@/db_type\tuserdb\n"
         "#@/user_id\tpeer\n"
         "#@/tick\t1\n"
         "remote\tRemote word\tc=2 d=1 t=1\n";
  ASSERT_TRUE(manager.SynchronizeAll());
  ExpectRecord("luna", "local \tLocal word");
  ExpectRecord("luna", "remote \tRemote word");
  EXPECT_FALSE(fs::exists(root_ / "sync" / "this-device" / ".temp.userdb.txt"));
}

TEST_F(UserDictSyncTest, CorruptUserDictionaryStillFailsAndOthersAreBackedUp) {
  CreateDictionary("luna", "local \tLocal word");
  ASSERT_TRUE(fs::create_directory(root_ / ".temp.userdb"));
  ASSERT_TRUE(fs::create_directory(root_ / "broken.userdb"));
  std::ofstream(root_ / "broken.userdb" / "CURRENT") << "not-a-manifest\n";
  RootedUserDictManager manager(&deployer_, &component_);

  EXPECT_FALSE(manager.SynchronizeAll());
  ExpectRecord("luna", "local \tLocal word");
  EXPECT_TRUE(fs::exists(root_ / "broken.userdb" / "CURRENT"));
  EXPECT_TRUE(
      fs::is_regular_file(root_ / "sync" / "this-device" / "luna.userdb.txt"));
}
