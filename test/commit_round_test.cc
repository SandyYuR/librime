//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Regression tests for commit rounds: several Memory instances that share one
// user dictionary must contribute to a single transaction per commit, so that
// an immediate BackSpace reverts the whole round instead of only its own share.
//
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/user_dictionary.h>
#include <rime/dict/vocabulary.h>
#include <rime/engine.h>
#include <rime/gear/memory.h>
#include <rime/gear/translator_commons.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/language.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/ticket.h>
#include <rime/translation.h>

using namespace rime;

namespace {

const char* kDbName = "commit_round_test";
const char* kCode = "ceshi";
const char* kText = "测试";

// A minimal learning component: it records every committed phrase into the
// user dictionary, exactly like the translators do through CommitEntry::Save.
class TestMemory : public Memory {
 public:
  explicit TestMemory(const Ticket& ticket) : Memory(ticket) {}
  bool Memorize(const CommitEntry& entry) override {
    if (!user_dict())
      return false;
    DictEntry e = entry;
    e.custom_code = kCode;
    return user_dict()->UpdateEntry(e, 1);
  }
};

}  // namespace

class CommitRoundTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RemoveDbFiles();
    engine_.reset(Engine::Create());
    Config* config = new Config;
    config->SetString("translator/user_dict", kDbName);
    config->SetBool("translator/enable_user_dict", true);
    engine_->ApplySchema(new Schema("commit_round_test", config));
  }

  void TearDown() override {
    memories_.clear();
    engine_.reset();
    RemoveDbFiles();
  }

  TestMemory* AddMemory() {
    memories_.emplace_back(
        std::make_unique<TestMemory>(Ticket(engine_.get(), "translator")));
    return memories_.back().get();
  }

  // Simulates one commit of a single confirmed segment.
  void CommitOnce() {
    ASSERT_FALSE(memories_.empty());
    Context* ctx = engine_->context();
    ctx->set_composition(Composition());
    ctx->set_input(kCode);
    auto entry = New<DictEntry>();
    entry->text = kText;
    entry->custom_code = kCode;
    auto menu = New<Menu>();
    menu->AddTranslation(New<UniqueTranslation>(New<Phrase>(
        memories_.front()->language(), kDbName, 0, strlen(kCode), entry)));
    Segment seg(0, strlen(kCode));
    seg.status = Segment::kConfirmed;
    seg.menu = menu;
    seg.selected_index = 0;
    ctx->composition().push_back(seg);
    ctx->Commit();
    EXPECT_FALSE(ctx->IsComposing());
  }

  // An unhandled BackSpace, the key that is supposed to undo the commit.
  void PressBackspace() {
    Context* ctx = engine_->context();
    KeyEvent key(XK_BackSpace, 0);
    ctx->unhandled_key_notifier()(ctx, key);
  }

  // The number of committed records readable from the database file.
  // UserDictionary::LookupWords() goes through the Db accessor, which only
  // sees committed data: entries still sitting in an open transaction are
  // invisible here. This is what tells "already written" from "still pending".
  int CommittedRecords() {
    if (memories_.empty())
      return 0;
    UserDictEntryIterator iter;
    memories_.front()->user_dict()->LookupWords(&iter, kCode, false, 0,
                                                nullptr);
    int count = 0;
    while (!iter.exhausted()) {
      ++count;
      iter.Next();
    }
    return count;
  }

  int CommittedCommitCount() {
    if (memories_.empty())
      return 0;
    UserDictEntryIterator iter;
    memories_.front()->user_dict()->LookupWords(&iter, kCode, false, 0,
                                                nullptr);
    return iter.exhausted() ? 0 : iter.Peek()->commit_count;
  }

  static std::filesystem::path DbPath() {
    return std::filesystem::current_path() / (std::string(kDbName) + ".userdb");
  }

  static void RemoveDbFiles() {
    std::error_code ec;
    std::filesystem::remove_all(DbPath(), ec);
  }

  the<Engine> engine_;
  std::vector<std::unique_ptr<TestMemory>> memories_;
};

// One learner: the commit stays pending until the next round or BackSpace.
TEST_F(CommitRoundTest, SingleMemoryKeepsCommitPending) {
  AddMemory();
  CommitOnce();
  EXPECT_EQ(0, CommittedRecords());
}

TEST_F(CommitRoundTest, SingleMemoryRollsBackOnBackspace) {
  AddMemory();
  CommitOnce();
  PressBackspace();
  EXPECT_EQ(0, CommittedRecords());
}

// The regression: a second Memory joining the same round must not commit the
// first one's pending entries by opening a transaction of its own.
TEST_F(CommitRoundTest, SecondMemoryDoesNotCommitFirstOnesRound) {
  AddMemory();
  AddMemory();
  CommitOnce();
  EXPECT_EQ(0, CommittedRecords());
}

TEST_F(CommitRoundTest, TwoMemoriesRollBackTogetherOnBackspace) {
  AddMemory();
  AddMemory();
  CommitOnce();
  PressBackspace();
  EXPECT_EQ(0, CommittedRecords());
}

TEST_F(CommitRoundTest, ThreeMemoriesRollBackTogetherOnBackspace) {
  AddMemory();
  AddMemory();
  AddMemory();
  CommitOnce();
  EXPECT_EQ(0, CommittedRecords());
  PressBackspace();
  EXPECT_EQ(0, CommittedRecords());
}

// Consecutive commits: the next round commits the previous one, and BackSpace
// only reverts the latest round.
TEST_F(CommitRoundTest, ConsecutiveRoundsCommitOnlyThePreviousOne) {
  AddMemory();
  AddMemory();
  CommitOnce();
  CommitOnce();
  // The first round has been closed by the second one, the second is pending.
  EXPECT_EQ(1, CommittedRecords());
  EXPECT_EQ(2, CommittedCommitCount());
  PressBackspace();
  EXPECT_EQ(1, CommittedRecords());
  EXPECT_EQ(2, CommittedCommitCount());
}

// The rollback window expires; after it BackSpace must not delete anything.
TEST_F(CommitRoundTest, BackspaceAfterRollbackWindowKeepsTheCommit) {
  AddMemory();
  AddMemory();
  CommitOnce();
  std::this_thread::sleep_for(std::chrono::seconds(4));
  PressBackspace();
  EXPECT_EQ(1, CommittedRecords());
}
