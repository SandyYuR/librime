#include <gtest/gtest.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/language.h>
#include <rime/menu.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/dict/level_db.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/memory.h>
#include <rime/gear/translator_commons.h>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace rime;

namespace {

const string kCode = "test code ";
const string kFirst = "第一词";
const string kSecond = "第二词";

class TransactionDb : public UserDbWrapper<LevelDb> {
 public:
  using UserDbWrapper<LevelDb>::UserDbWrapper;
  bool BeginTransaction() override {
    ++begins;
    return LevelDb::BeginTransaction();
  }
  bool CommitTransaction() override {
    ++commits;
    return LevelDb::CommitTransaction();
  }
  int begins = 0;
  int commits = 0;
};

// Exercise Memory's real signal connections with a deterministic writer and
// an observer equivalent to a LuaMemory without a memorize callback.
class TestMemory : public Memory {
 public:
  TestMemory(Engine* engine, const an<Db>& db, bool writes)
      : Memory(MakeTicket(engine)), writes_(writes) {
    user_dict_.reset(new UserDictionary(db->name(), db));
    EXPECT_TRUE(user_dict_->Load());
    language_.reset(new Language(db->name()));
  }
  bool Memorize(const CommitEntry& committed) override {
    if (!writes_)
      return false;
    DictEntry entry;
    entry.text = committed.text;
    entry.custom_code = kCode;
    return user_dict_->UpdateEntry(entry, 1);
  }

 private:
  static Ticket MakeTicket(Engine* engine) {
    Ticket ticket;
    ticket.engine = engine;
    return ticket;
  }
  bool writes_;
};

class MemoryTransactionTest : public testing::Test {
 protected:
  void SetUp() override {
    db = New<TransactionDb>(path{"memory_transaction_test.userdb"},
                            "memory_transaction_test");
    if (db->Exists())
      ASSERT_TRUE(db->Remove());
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->MetaUpdate("/tick", "100"));
    engine.reset(Engine::Create());
  }
  void TearDown() override {
    engine.reset();
    ASSERT_TRUE(db->Close());
    ASSERT_TRUE(db->Remove());
  }
  void Commit(Engine* target, Memory& writer, const string& text) {
    auto entry = New<DictEntry>();
    entry->text = text;
    entry->custom_code = kCode;
    auto phrase = New<Phrase>(writer.language(), "phrase", 0, 1, entry);
    Segment seg(0, 1);
    seg.status = Segment::kConfirmed;
    seg.menu = New<Menu>();
    seg.menu->AddTranslation(New<UniqueTranslation>(phrase));
    target->context()->composition().push_back(seg);
    ASSERT_TRUE(target->context()->Commit());
  }
  bool Persisted(const string& text) {
    string value;
    return db->Fetch(kCode + '\t' + text, &value);
  }
  int Count(Memory& memory, const string& text) {
    UserDictEntryIterator iter;
    memory.user_dict()->LookupWords(&iter, kCode.substr(0, kCode.size() - 1),
                                    false);
    while (!iter.exhausted()) {
      auto entry = iter.Peek();
      if (entry && entry->text == text)
        return entry->commit_count;
      iter.Next();
    }
    return 0;
  }
  void Backspace(Engine* target) {
    EXPECT_FALSE(target->ProcessKey(KeyEvent(XK_BackSpace, 0)));
  }
  an<TransactionDb> db;
  the<Engine> engine;
};

TEST_F(MemoryTransactionTest, SingleMemoryCanUndoLearning) {
  TestMemory writer(engine.get(), db, true);
  Commit(engine.get(), writer, kFirst);
  EXPECT_FALSE(Persisted(kFirst));
  Backspace(engine.get());
  EXPECT_FALSE(Persisted(kFirst));
  EXPECT_EQ(0, Count(writer, kFirst));
}

TEST_F(MemoryTransactionTest, ObserverMustNotCommitTheWriter) {
  TestMemory writer(engine.get(), db, true);
  TestMemory observer(engine.get(), db, false);
  Commit(engine.get(), writer, kFirst);
  EXPECT_EQ(1, db->begins);
  EXPECT_EQ(0, db->commits);
  EXPECT_FALSE(Persisted(kFirst));
  Backspace(engine.get());
  EXPECT_EQ(0, Count(writer, kFirst));
  EXPECT_EQ(0, Count(observer, kFirst));
  EXPECT_FALSE(Persisted(kFirst));
}

TEST_F(MemoryTransactionTest, MultipleWritersRollBackTogether) {
  TestMemory first(engine.get(), db, true);
  TestMemory second(engine.get(), db, true);
  Commit(engine.get(), first, kFirst);
  EXPECT_EQ(102, first.user_dict()->tick());
  Backspace(engine.get());
  EXPECT_EQ(0, Count(first, kFirst));
  EXPECT_EQ(0, Count(second, kFirst));
  EXPECT_FALSE(Persisted(kFirst));
}

TEST_F(MemoryTransactionTest, JoinedWritersBothContributeWhenCommitted) {
  TestMemory first(engine.get(), db, true);
  TestMemory second(engine.get(), db, true);
  Commit(engine.get(), first, kFirst);
  EXPECT_FALSE(Persisted(kFirst));
  EXPECT_FALSE(engine->ProcessKey(KeyEvent(XK_space, 0)));
  EXPECT_EQ(2, Count(first, kFirst));
  EXPECT_EQ(2, Count(second, kFirst));
}

TEST_F(MemoryTransactionTest, ObserverRegisteredFirstJoinsTheSameTransaction) {
  TestMemory observer(engine.get(), db, false);
  TestMemory writer(engine.get(), db, true);
  Commit(engine.get(), writer, kFirst);
  EXPECT_EQ(1, db->begins);
  EXPECT_EQ(0, db->commits);
  Backspace(engine.get());
  EXPECT_EQ(0, Count(writer, kFirst));
}

TEST_F(MemoryTransactionTest, RepeatedWordRestoresItsPreviousFrequency) {
  TestMemory writer(engine.get(), db, true);
  TestMemory observer(engine.get(), db, false);
  Commit(engine.get(), writer, kFirst);
  Commit(engine.get(), writer, kFirst);
  EXPECT_EQ(102, writer.user_dict()->tick());
  Backspace(engine.get());
  EXPECT_TRUE(Persisted(kFirst));
  EXPECT_EQ(1, Count(writer, kFirst));
  EXPECT_EQ(1, Count(observer, kFirst));
}

TEST_F(MemoryTransactionTest, SpaceAfterBackspaceDoesNotRestoreLearning) {
  TestMemory writer(engine.get(), db, true);
  TestMemory observer(engine.get(), db, false);
  Commit(engine.get(), writer, kFirst);
  Backspace(engine.get());
  EXPECT_FALSE(engine->ProcessKey(KeyEvent(XK_space, 0)));
  EXPECT_EQ(0, Count(writer, kFirst));
  EXPECT_FALSE(Persisted(kFirst));
}

TEST_F(MemoryTransactionTest, UnhandledSpaceFinishesLearning) {
  TestMemory writer(engine.get(), db, true);
  TestMemory observer(engine.get(), db, false);
  Commit(engine.get(), writer, kFirst);
  EXPECT_FALSE(Persisted(kFirst));
  EXPECT_FALSE(engine->ProcessKey(KeyEvent(XK_space, 0)));
  Backspace(engine.get());
  EXPECT_TRUE(Persisted(kFirst));
  EXPECT_EQ(1, Count(writer, kFirst));
}

TEST_F(MemoryTransactionTest, ExpiredTransactionKeepsLearning) {
  TestMemory writer(engine.get(), db, true);
  TestMemory observer(engine.get(), db, false);
  Commit(engine.get(), writer, kFirst);
  EXPECT_FALSE(Persisted(kFirst));
  std::this_thread::sleep_for(std::chrono::seconds(4));
  Backspace(engine.get());
  EXPECT_TRUE(Persisted(kFirst));
  EXPECT_EQ(1, Count(writer, kFirst));
}

TEST_F(MemoryTransactionTest, ConsecutiveCommitsUndoOnlyTheLastWord) {
  TestMemory writer(engine.get(), db, true);
  TestMemory observer(engine.get(), db, false);
  Commit(engine.get(), writer, kFirst);
  Commit(engine.get(), writer, kSecond);
  EXPECT_TRUE(Persisted(kFirst));
  EXPECT_FALSE(Persisted(kSecond));
  Backspace(engine.get());
  EXPECT_EQ(1, Count(writer, kFirst));
  EXPECT_EQ(0, Count(writer, kSecond));
}

TEST_F(MemoryTransactionTest, OtherContextCannotUndoOrFinishTheLatestCommit) {
  TestMemory first(engine.get(), db, true);
  the<Engine> other(Engine::Create());
  TestMemory second(other.get(), db, true);
  Commit(engine.get(), first, kFirst);
  Commit(other.get(), second, kSecond);
  EXPECT_TRUE(Persisted(kFirst));
  Backspace(engine.get());
  EXPECT_TRUE(db->in_transaction());
  EXPECT_EQ(102, second.user_dict()->tick());
  EXPECT_FALSE(Persisted(kSecond));
  Backspace(other.get());
  EXPECT_EQ(0, Count(second, kSecond));
  EXPECT_EQ(1, Count(first, kFirst));
}

TEST_F(MemoryTransactionTest, DifferentDatabasesHaveIndependentTransactions) {
  auto other_db =
      New<TransactionDb>(path{"memory_other_test.userdb"}, "memory_other_test");
  if (other_db->Exists())
    ASSERT_TRUE(other_db->Remove());
  ASSERT_TRUE(other_db->Open());
  ASSERT_TRUE(other_db->MetaUpdate("/tick", "100"));
  {
    TestMemory first(engine.get(), db, true);
    TestMemory second(engine.get(), other_db, false);
    Commit(engine.get(), first, kFirst);
    EXPECT_EQ(1, db->begins);
    EXPECT_EQ(1, other_db->begins);
    Backspace(engine.get());
    EXPECT_FALSE(db->in_transaction());
    EXPECT_FALSE(other_db->in_transaction());
    EXPECT_FALSE(Persisted(kFirst));
  }
  ASSERT_TRUE(other_db->Close());
  ASSERT_TRUE(other_db->Remove());
}

TEST_F(MemoryTransactionTest, ManualCommitCallbackCanLearnAndUndo) {
  TestMemory memory(engine.get(), db, false);
  auto connection = engine->context()->commit_notifier().connect([&](Context*) {
    DictEntry entry;
    entry.text = kFirst;
    entry.custom_code = kCode;
    EXPECT_TRUE(memory.user_dict()->UpdateEntry(entry, 1));
  });
  Commit(engine.get(), memory, kFirst);
  EXPECT_EQ(101, memory.user_dict()->tick());
  Backspace(engine.get());
  EXPECT_EQ(0, Count(memory, kFirst));
  EXPECT_FALSE(Persisted(kFirst));
  connection.disconnect();
}

TEST_F(MemoryTransactionTest, ManualWriterJoinsTheNativeLearnersTransaction) {
  TestMemory writer(engine.get(), db, true);
  TestMemory manual(engine.get(), db, false);
  auto connection = engine->context()->commit_notifier().connect([&](Context*) {
    DictEntry entry;
    entry.text = kSecond;
    entry.custom_code = kCode;
    EXPECT_TRUE(manual.user_dict()->UpdateEntry(entry, 1));
  });
  Commit(engine.get(), writer, kFirst);
  EXPECT_EQ(102, writer.user_dict()->tick());
  EXPECT_FALSE(Persisted(kFirst));
  EXPECT_FALSE(Persisted(kSecond));
  Backspace(engine.get());
  EXPECT_EQ(0, Count(writer, kFirst));
  EXPECT_EQ(0, Count(manual, kSecond));
  EXPECT_FALSE(Persisted(kFirst));
  EXPECT_FALSE(Persisted(kSecond));
  connection.disconnect();
}

TEST_F(MemoryTransactionTest, NontransactionalDictionaryStillLearns) {
  auto text_db = New<UserDbWrapper<TextDb>>(path{"memory_text_test.txt"},
                                            "memory_text_test");
  if (text_db->Exists())
    ASSERT_TRUE(text_db->Remove());
  ASSERT_TRUE(text_db->Open());
  ASSERT_TRUE(text_db->MetaUpdate("/tick", "100"));
  {
    TestMemory writer(engine.get(), text_db, true);
    Commit(engine.get(), writer, kFirst);
    string value;
    EXPECT_TRUE(text_db->Fetch(kCode + '\t' + kFirst, &value));
  }
  ASSERT_TRUE(text_db->Close());
  ASSERT_TRUE(text_db->Remove());
}

TEST(ContextCommitTest, CommitIdsAreScopedAndAdvanceForIdenticalCommits) {
  Context context;
  Context other;
  EXPECT_NE(context.context_id(), other.context_id());
  EXPECT_EQ(0, context.commit_id());
  vector<uint64_t> ids;
  context.commit_notifier().connect([&](Context* ctx) {
    EXPECT_NE(0, ctx->commit_id());
    ids.push_back(ctx->commit_id());
  });
  for (int i = 0; i < 2; ++i) {
    context.composition().emplace_back(0, 1);
    EXPECT_TRUE(context.Commit());
    EXPECT_EQ(0, context.commit_id());
  }
  ASSERT_EQ(2, ids.size());
  EXPECT_GT(ids[1], ids[0]);
}

TEST(ContextCommitTest, NestedNotificationRestoresTheOuterCommitId) {
  Context context;
  uint64_t outer = 0, inner = 0;
  context.commit_notifier().connect([&](Context* ctx) {
    if (!outer) {
      outer = ctx->commit_id();
      EXPECT_TRUE(ctx->Commit());
      EXPECT_EQ(outer, ctx->commit_id());
    } else {
      inner = ctx->commit_id();
    }
  });
  context.composition().emplace_back(0, 1);
  EXPECT_TRUE(context.Commit());
  EXPECT_NE(outer, inner);
  EXPECT_NE(0, inner);
  EXPECT_EQ(0, context.commit_id());
}

TEST(ContextCommitTest, ThrowingNotificationDoesNotLeakTheCommitId) {
  Context context;
  auto connection = context.commit_notifier().connect(
      [](Context*) { throw std::runtime_error("notification failure"); });
  context.composition().emplace_back(0, 1);
  EXPECT_THROW(context.Commit(), std::runtime_error);
  EXPECT_EQ(0, context.commit_id());
  connection.disconnect();
  EXPECT_TRUE(context.Commit());
  EXPECT_EQ(0, context.commit_id());
}

}  // namespace
