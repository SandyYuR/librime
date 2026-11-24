//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Literal T9 tabs and fuzzy candidates have different contracts: tabs describe
// the keys that were typed, while selecting one retains the schema's fuzzy
// syllables and phrases. Keep their assertions in separate tests so a missing
// tab cannot hide a candidate-filtering regression.
//
// The complete algebra below comes from the user-supplied
// wanxiang_t9.custom.yaml, SHA-256:
// 06c3c40bf3b6b4fd82f69939ffdd2f8a86607b9d56cae2763f36d784cbd1b1e0.
// It preserves rule order, tone handling, both fuzzy groups and the uppercase
// derive used for T9 encoding. No external schema or user data is required.

#include <gtest/gtest.h>
#include <rime_api.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/prism.h>
#include <rime/dict/table.h>
#include <rime/key_table.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kSchemaId = "literal_wanxiang_tab_test";
constexpr const char* kSchemaFile = "literal_wanxiang_tab_test.schema.yaml";
constexpr const char* kDictionaryName = "dictionary_literal_wanxiang_tab_test";
constexpr const char* kDictionaryFile =
    "dictionary_literal_wanxiang_tab_test.dict.yaml";

constexpr const char* kSchema = R"YAML(# encoding: utf-8
schema:
  schema_id: literal_wanxiang_tab_test
  name: literal Wanxiang T9 tabs
  version: "1"
engine:
  processors: [speller]
  segmentors: [abc_segmentor]
  translators: [script_translator]
speller:
  alphabet: "1234567890"
  delimiter: " '"
  algebra:
    - xform/ⅱ//
    - xform/^(.*);.*$/$1/
    - derive/^([zcs])h/$1/
    - derive/^([zcs])([^h])/$1h$2/
    - derive/([ēéěèe])ng$/$1n/
    - derive/([ēéěèe])n$/$1ng/
    - derive/([īíǐìi])ng$/$1n/
    - derive/([īíǐìi])n$/$1ng/
    - derive/([^jqxy])u([āáǎàa])ng$/$1u$2n/
    - derive/([^jqxy])u([āáǎàa])n$/$1u$2ng/
    - derive/([^iuy])([āáǎàa])ng$/$1$2n/
    - derive/([^iuy])([āáǎàa])n$/$1$2ng/
    - derive/^([āáǎàa])ng$/$1n/
    - derive/^([āáǎàa])n$/$1ng/
    - xlit/āáǎàōóǒòēéěèīíǐìūúǔùǖǘǚǜüńňǹ/aaaaooooeeeeiiiiuuuuvvvvvnnn/
    - xform/ḿ/me/
    - xform/m̀/me/
    - derive/^ng$/eng/
    - xform/^n$/en/
    - derive/^(.*)$/\U$1/
    - derive/^([nl])ve$/$1ue/
    - derive/^([NL])VE$/$1UE/
    - derive/ong$/on/
    - derive/iong$/ion/
    - derive/ONG$/ON/
    - derive/IONG$/ION/
    - derive/^([jqxy])u/$1v/
    - derive/^([JQXY])U/$1V/
    - xlit/ABCDEFGHIJKLMNOPQRSTUVWXYZ/22233344455566677778889999/
translator:
  dictionary: dictionary_literal_wanxiang_tab_test
  enable_completion: true
  enable_correction: false
  enable_user_dict: false
menu:
  page_size: 20
)YAML";

// These original dictionary codes retain their tones. In particular, neither
// zhuan nor zhuang is silently replaced with the already-normalized "zuan".
constexpr const char* kDictionary = R"YAML(# encoding: utf-8
---
name: dictionary_literal_wanxiang_tab_test
version: "0.1"
sort: by_weight
...
转	zhuǎn	100
专	zhuān	100
装	zhuāng	100
钻	zuān	100
速	sù	100
属	shǔ	100
转速	zhuǎn sù	1000
专属	zhuān shǔ	1000
)YAML";

struct LiteralTab {
  std::string label;
  size_t span;
  int source;
};

std::string DescribeTabs(const std::vector<LiteralTab>& tabs) {
  std::string result;
  for (const auto& tab : tabs) {
    if (!result.empty())
      result += '|';
    result += tab.label + ':' + std::to_string(tab.span);
  }
  return result;
}

std::vector<std::string> FullSyllableTabs(const std::vector<LiteralTab>& tabs) {
  std::vector<std::string> result;
  for (const auto& tab : tabs) {
    // InputTabEntry::kSyllable == 0. Exclude the independent one-key initial
    // and raw-input tabs when checking the complete literal syllable list.
    if (tab.source == 0 && tab.span > 1)
      result.push_back(tab.label + ':' + std::to_string(tab.span));
  }
  return result;
}

class LiteralTabSession {
 public:
  LiteralTabSession() : api_(rime_get_api()) {}
  ~LiteralTabSession() {
    if (session_)
      api_->destroy_session(session_);
  }

  bool Open(const char* schema_id = kSchemaId) {
    session_ = api_->create_session();
    return session_ && api_->select_schema(session_, schema_id);
  }

  void Type(const std::string& keys) {
    for (const unsigned char key : keys)
      api_->process_key(session_, key, 0);
  }

  std::vector<LiteralTab> Tabs() {
    char** labels = nullptr;
    size_t* spans = nullptr;
    int* sources = nullptr;
    size_t count = 0;
    std::vector<LiteralTab> tabs;
    if (!api_->get_input_tabs(session_, 0, &labels, &spans, &sources, &count))
      return tabs;
    for (size_t i = 0; i < count; ++i)
      tabs.push_back({labels[i] ? labels[i] : "", spans[i], sources[i]});
    api_->free_input_tabs(labels, spans, sources, count);
    return tabs;
  }

  bool SelectTab(const char* label, size_t span) {
    // Position zero asks the native API to use its current tab cursor. After
    // selecting zuan:4 this must select the next syllable, not position zero.
    return api_->select_tab(session_, 0, label, span);
  }

  bool Backspace() { return api_->process_key(session_, XK_BackSpace, 0); }

  void Clear() { api_->clear_composition(session_); }

  std::string Input() const {
    const char* input = api_->get_input(session_);
    return input ? input : "";
  }

  std::set<std::string> CandidateSet() {
    const auto candidates = Candidates();
    return {candidates.begin(), candidates.end()};
  }

  std::vector<std::string> Candidates() {
    std::vector<std::string> candidates;
    RimeCandidateListIterator iterator{};
    if (!api_->candidate_list_begin(session_, &iterator))
      return candidates;
    while (api_->candidate_list_next(&iterator)) {
      if (iterator.candidate.text)
        candidates.emplace_back(iterator.candidate.text);
    }
    api_->candidate_list_end(&iterator);
    return candidates;
  }

 private:
  RimeApi* api_;
  RimeSessionId session_ = 0;
};

class ScriptTranslatorLiteralTab : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    // The native test environment places resources in its isolated host test
    // directory. Use names owned by this suite and never read a user profile.
    {
      std::ofstream dictionary(kDictionaryFile);
      ASSERT_TRUE(dictionary.is_open());
      dictionary << kDictionary;
      ASSERT_TRUE(dictionary.good());
    }
    {
      std::ofstream schema(kSchemaFile);
      ASSERT_TRUE(schema.is_open());
      schema << kSchema;
      ASSERT_TRUE(schema.good());
    }
    const std::string name = kDictionaryName;
    rime::the<rime::Dictionary> dictionary(new rime::Dictionary(
        name, {}, {rime::New<rime::Table>(rime::path{name + ".table.bin"})},
        rime::New<rime::Prism>(rime::path{name + ".prism.bin"})));
    dictionary->Remove();
    rime::DictCompiler compiler(dictionary.get());
    ASSERT_TRUE(compiler.Compile(rime::path{kSchemaFile}));
    ASSERT_TRUE(dictionary->Load());
  }

  void SetUp() override { ASSERT_TRUE(session_.Open()); }

  void ExpectCandidate(const std::string& expected) {
    const auto candidates = session_.Candidates();
    EXPECT_NE(candidates.end(),
              std::find(candidates.begin(), candidates.end(), expected))
        << "Missing exact candidate " << expected
        << "; candidates: " << testing::PrintToString(candidates);
  }

  LiteralTabSession session_;
};

TEST_F(ScriptTranslatorLiteralTab, ShowsOnlyZuanForLiteralZuanKeys) {
  session_.Type("9826");
  const auto tabs = session_.Tabs();
  EXPECT_EQ((std::vector<std::string>{"zuan:4"}), FullSyllableTabs(tabs))
      << DescribeTabs(tabs);
}

TEST_F(ScriptTranslatorLiteralTab, ShowsOnlyZhuanForLiteralZhuanKeys) {
  session_.Type("94826");
  const auto tabs = session_.Tabs();
  EXPECT_EQ((std::vector<std::string>{"zhuan:5"}), FullSyllableTabs(tabs))
      << DescribeTabs(tabs);
}

TEST_F(ScriptTranslatorLiteralTab, ShowsLiteralSuAtTheNextTabCursor) {
  session_.Type("982678");
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  const auto tabs = session_.Tabs();
  EXPECT_EQ((std::vector<std::string>{"su:2"}), FullSyllableTabs(tabs))
      << DescribeTabs(tabs);
}

TEST_F(ScriptTranslatorLiteralTab, ShowsLiteralShuAtTheNextTabCursor) {
  session_.Type("9826748");
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  const auto tabs = session_.Tabs();
  EXPECT_EQ((std::vector<std::string>{"shu:3"}), FullSyllableTabs(tabs))
      << DescribeTabs(tabs);
}

TEST_F(ScriptTranslatorLiteralTab, ZuanSelectionKeepsFuzzySingleSyllables) {
  session_.Type("9826");
  for (const auto* candidate : {"转", "专", "装", "钻"})
    ExpectCandidate(candidate);
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  for (const auto* candidate : {"转", "专", "装", "钻"})
    ExpectCandidate(candidate);
}

TEST_F(ScriptTranslatorLiteralTab, ZuanSelectionKeepsZhuanSuPhrase) {
  session_.Type("982678");
  ExpectCandidate("转速");
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  ExpectCandidate("转速");
}

TEST_F(ScriptTranslatorLiteralTab, ZuanSelectionKeepsZhuanShuPhrase) {
  session_.Type("9826748");
  ExpectCandidate("专属");
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  ExpectCandidate("专属");
}

TEST_F(ScriptTranslatorLiteralTab, SelectingNextSuKeepsZhuanSuPhrase) {
  session_.Type("982678");
  ExpectCandidate("转速");
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  ASSERT_TRUE(session_.SelectTab("su", 2));
  ExpectCandidate("转速");
}

TEST_F(ScriptTranslatorLiteralTab, SelectingNextShuKeepsZhuanShuPhrase) {
  session_.Type("9826748");
  ExpectCandidate("专属");
  ASSERT_TRUE(session_.SelectTab("zuan", 4));
  ASSERT_TRUE(session_.SelectTab("shu", 3));
  ExpectCandidate("专属");
}

// Boundary schemas deliberately use plain dictionary codes and distinct names.
// They do not change the original, tone-marked Wanxiang fixture above.
void BuildLiteralBoundaryResources(const std::string& schema_id,
                                   const std::string& alphabet,
                                   const std::string& algebra,
                                   bool completion) {
  const std::string name = schema_id + "_dict";
  const std::string schema_file = schema_id + ".schema.yaml";
  {
    std::ofstream dictionary(name + ".dict.yaml");
    ASSERT_TRUE(dictionary.is_open());
    dictionary << "# encoding: utf-8\n---\nname: " << name
               << "\nversion: '0.1'\nsort: by_weight\n...\n"
                  "转\tzhuan\t100\n装\tzhuang\t100\n";
    ASSERT_TRUE(dictionary.good());
  }
  {
    std::ofstream schema(schema_file);
    ASSERT_TRUE(schema.is_open());
    schema << "schema:\n  schema_id: " << schema_id
           << "\n  name: literal tab boundary\n  version: '1'\n"
              "engine:\n  processors: [speller]\n"
              "  segmentors: [abc_segmentor]\n"
              "  translators: [script_translator]\n"
              "speller:\n  alphabet: '"
           << alphabet << "'\n";
    if (algebra.empty())
      schema << "  algebra: []\n";
    else
      schema << "  algebra:\n" << algebra;
    schema << "translator:\n  dictionary: " << name
           << "\n  enable_completion: " << (completion ? "true" : "false")
           << "\n  enable_correction: false\n  enable_user_dict: false\n"
              "menu:\n  page_size: 20\n";
    ASSERT_TRUE(schema.good());
  }
  rime::the<rime::Dictionary> dictionary(new rime::Dictionary(
      name, {}, {rime::New<rime::Table>(rime::path{name + ".table.bin"})},
      rime::New<rime::Prism>(rime::path{name + ".prism.bin"})));
  dictionary->Remove();
  rime::DictCompiler compiler(dictionary.get());
  ASSERT_TRUE(compiler.Compile(rime::path{schema_file}));
  ASSERT_TRUE(dictionary->Load());
}

TEST(ScriptTranslatorLiteralBoundary,
     PureXlitWithoutFuzzyRulesKeepsExistingCompletionTabs) {
  constexpr const char* schema_id = "literal_t9_completion_boundary";
  ASSERT_NO_FATAL_FAILURE(BuildLiteralBoundaryResources(
      schema_id, "1234567890",
      "    - xlit/abcdefghijklmnopqrstuvwxyz/22233344455566677778889999/\n",
      true));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(schema_id));
  session.Type("9482");  // zhua: neither zhuan nor zhuang was fully typed.

  const auto tabs = session.Tabs();
  EXPECT_EQ((std::vector<std::string>{"zhuan:4", "zhuang:4"}),
            FullSyllableTabs(tabs))
      << DescribeTabs(tabs);
  EXPECT_EQ("zhuan:4|zhuang:4|w:1|x:1|y:1|z:1|9:1", DescribeTabs(tabs));
  const auto candidates = session.Candidates();
  for (const auto* expected : {"转", "装"}) {
    EXPECT_NE(candidates.end(),
              std::find(candidates.begin(), candidates.end(), expected))
        << testing::PrintToString(candidates);
  }
}

TEST(ScriptTranslatorLiteralBoundary,
     AbbreviationWithoutFuzzyRulesKeepsExistingTabs) {
  constexpr const char* schema_id = "literal_t9_abbrev_boundary";
  ASSERT_NO_FATAL_FAILURE(BuildLiteralBoundaryResources(
      schema_id, "1234567890",
      "    - abbrev/^([a-z]{2}).*$/$1/\n"
      "    - xlit/abcdefghijklmnopqrstuvwxyz/22233344455566677778889999/\n",
      false));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(schema_id));
  session.Type("94");  // zh is an abbreviation of both dictionary syllables.

  const auto tabs = session.Tabs();
  EXPECT_EQ((std::vector<std::string>{"zhuan:2", "zhuang:2"}),
            FullSyllableTabs(tabs))
      << DescribeTabs(tabs);
  EXPECT_EQ("zhuan:2|zhuang:2|w:1|x:1|y:1|z:1|9:1", DescribeTabs(tabs));
  ASSERT_FALSE(tabs.empty());
  EXPECT_EQ(3, tabs.back().source);  // InputTabEntry::kRawInput.
  for (size_t i = 0; i + 1 < tabs.size(); ++i)
    EXPECT_EQ(0, tabs[i].source);  // InputTabEntry::kSyllable initials.
  const auto candidates = session.Candidates();
  for (const auto* expected : {"转", "装"}) {
    EXPECT_NE(candidates.end(),
              std::find(candidates.begin(), candidates.end(), expected))
        << testing::PrintToString(candidates);
  }
}

// Letter-key input gets no syllable tabs at all; the bar is digit-key only.
// Completion still applies, so "zhua" reaches zhuan/zhuang.
TEST(ScriptTranslatorLiteralBoundary, NonDigitPinyinHasNoTabs) {
  constexpr const char* schema_id = "literal_pinyin_completion_boundary";
  ASSERT_NO_FATAL_FAILURE(BuildLiteralBoundaryResources(
      schema_id, "abcdefghijklmnopqrstuvwxyz", "", true));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(schema_id));
  session.Type("zhua");

  const auto tabs = session.Tabs();
  EXPECT_TRUE(tabs.empty()) << DescribeTabs(tabs);
  const auto candidates = session.Candidates();
  for (const auto* expected : {"转", "装"}) {
    EXPECT_NE(candidates.end(),
              std::find(candidates.begin(), candidates.end(), expected))
        << testing::PrintToString(candidates);
  }
}

constexpr const char* kToneAlgebra =
    "    - xlit/āáǎàōóǒòēéěèīíǐìūúǔùǖǘǚǜüńňǹ/"
    "aaaaooooeeeeiiiiuuuuvvvvvnnn/\n";
constexpr const char* kUppercaseT9Algebra =
    "    - derive/^(.*)$/\\U$1/\n"
    "    - xlit/ABCDEFGHIJKLMNOPQRSTUVWXYZ/22233344455566677778889999/\n";
constexpr const char* kMinimalEntries =
    "赞\tzan\t100\n站\tzhan\t100\n脏\tzang\t100\n张\tzhang\t100\n"
    "湾\twan\t100\n烟\tyan\t100\n";

// This builder owns all its resource names in the host test directory. The
// default editor lets the interaction tests send real BackSpace key events.
void BuildTabResources(const std::string& id,
                       const std::string& entries,
                       const std::string& algebra,
                       const std::string& alphabet = "1234567890",
                       const std::string& delimiter = " '",
                       bool completion = false) {
  {
    std::ofstream dictionary(id + ".dict.yaml");
    ASSERT_TRUE(dictionary.is_open());
    dictionary << "# encoding: utf-8\n---\nname: " << id
               << "\nversion: '1'\nsort: by_weight\n...\n"
               << entries;
    ASSERT_TRUE(dictionary.good());
  }
  {
    std::ofstream schema(id + ".schema.yaml");
    ASSERT_TRUE(schema.is_open());
    schema << "schema:\n  schema_id: " << id
           << "\n  name: T9 regression\n  version: '1'\n"
              "engine:\n  processors: [speller, fluency_editor]\n"
              "  segmentors: [abc_segmentor]\n"
              "  translators: [script_translator]\n"
              "speller:\n  alphabet: '"
           << alphabet << "'\n  delimiter: \"" << delimiter
           << "\"\n  algebra:\n"
           << algebra << "translator:\n  dictionary: " << id
           << "\n  enable_user_dict: false\n  enable_completion: "
           << (completion ? "true" : "false")
           << "\n  enable_correction: false\nmenu:\n  page_size: 20\n";
    ASSERT_TRUE(schema.good());
  }
  rime::the<rime::Dictionary> dictionary(new rime::Dictionary(
      id, {}, {rime::New<rime::Table>(rime::path{id + ".table.bin"})},
      rime::New<rime::Prism>(rime::path{id + ".prism.bin"})));
  dictionary->Remove();
  rime::DictCompiler compiler(dictionary.get());
  ASSERT_TRUE(compiler.Compile(rime::path{id + ".schema.yaml"}));
  ASSERT_TRUE(dictionary->Load());
}

std::string MinimalAlgebra(bool finals_first) {
  const std::string initial = "    - derive/^zh/z/\n";
  const std::string final = "    - derive/([āáǎàa])ng$/$1n/\n";
  return (finals_first ? final + initial : initial + final) + kToneAlgebra +
         kUppercaseT9Algebra;
}

void CheckMinimalOneWayAlgebra(const char* schema_id, bool finals_first) {
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(schema_id, kMinimalEntries,
                                            MinimalAlgebra(finals_first)));
  struct Selection {
    const char* keys;
    const char* label;
    std::vector<std::string> tabs;
    std::set<std::string> candidates;
  };
  // These are directed derivations, not an unconditional four-way class.
  // Every choice below is a literal spelling of its actual input, in contrast
  // to forcing zhan:3 on 926 through the C API.
  const std::vector<Selection> selections = {
      {"926", "zan", {"wan:3", "yan:3", "zan:3"}, {"赞", "站", "脏", "张"}},
      {"926", "wan", {"wan:3", "yan:3", "zan:3"}, {"湾"}},
      {"926", "yan", {"wan:3", "yan:3", "zan:3"}, {"烟"}},
      {"9426", "zhan", {"zhan:4"}, {"站", "张"}},
      {"9264", "zang", {"zang:4"}, {"脏", "张"}},
      {"94264", "zhang", {"zhang:5"}, {"张"}},
  };
  for (const auto& selection : selections) {
    SCOPED_TRACE(std::string(selection.keys) + " -> " + selection.label);
    LiteralTabSession session;
    ASSERT_TRUE(session.Open(schema_id));
    session.Type(selection.keys);
    EXPECT_EQ(selection.tabs, FullSyllableTabs(session.Tabs()));
    ASSERT_TRUE(
        session.SelectTab(selection.label, std::string(selection.keys).size()));
    EXPECT_EQ(selection.candidates, session.CandidateSet());
  }
}

TEST(ScriptTranslatorLiteralTabRegression,
     OneWayInitialThenFinalKeepsAllReachableSyllables) {
  CheckMinimalOneWayAlgebra("literal_oneway_initial_final", false);
}

TEST(ScriptTranslatorLiteralTabRegression,
     OneWayFinalThenInitialKeepsAllReachableSyllables) {
  CheckMinimalOneWayAlgebra("literal_oneway_final_initial", true);
}

std::string UserAlgebra() {
  const std::string schema = kSchema;
  const std::string marker = "  algebra:\n";
  const size_t begin = schema.find(marker) + marker.size();
  return schema.substr(begin, schema.find("translator:\n", begin) - begin);
}

std::string UserEntries() {
  const std::string dictionary = kDictionary;
  const std::string marker = "\n...\n";
  return dictionary.substr(dictionary.find(marker) + marker.size());
}

TEST_F(ScriptTranslatorLiteralTab, ZhuangInputDoesNotOfferZuanOrDropZhuang) {
  session_.Type("948264");
  EXPECT_EQ((std::vector<std::string>{"zhuang:6"}),
            FullSyllableTabs(session_.Tabs()));
  ExpectCandidate("装");
  ASSERT_TRUE(session_.SelectTab("zhuang", 6));
  for (const auto* candidate : {"装", "转", "专", "钻"})
    ExpectCandidate(candidate);
}

TEST(ScriptTranslatorLiteralTabRegression,
     BackspaceUndoesSelectionBeforeDeletingKeysAndClearResetsTabs) {
  constexpr const char* id = "literal_backspace_clear";
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(id, UserEntries(), UserAlgebra()));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(id));
  session.Type("948264");
  ASSERT_TRUE(session.SelectTab("zhuang", 6));
  EXPECT_EQ("948264", session.Input());
  EXPECT_TRUE(FullSyllableTabs(session.Tabs()).empty());

  ASSERT_TRUE(session.Backspace());
  EXPECT_EQ("948264", session.Input());
  EXPECT_EQ((std::vector<std::string>{"zhuang:6"}),
            FullSyllableTabs(session.Tabs()));
  ASSERT_TRUE(session.Backspace());
  EXPECT_EQ("94826", session.Input());
  EXPECT_EQ((std::vector<std::string>{"zhuan:5"}),
            FullSyllableTabs(session.Tabs()));

  ASSERT_TRUE(session.SelectTab("zhuan", 5));
  session.Clear();
  EXPECT_TRUE(session.Input().empty());
  EXPECT_TRUE(session.Tabs().empty());
  EXPECT_TRUE(session.Candidates().empty());
  session.Type("9826");
  EXPECT_EQ((std::vector<std::string>{"zuan:4"}),
            FullSyllableTabs(session.Tabs()));
  ASSERT_TRUE(session.SelectTab("zuan", 4));
  EXPECT_EQ((std::set<std::string>{"转", "专", "装", "钻"}),
            session.CandidateSet());
}

TEST(ScriptTranslatorLiteralTabRegression,
     LongPhraseSurvivesEveryLiteralSelectionAndUndo) {
  constexpr const char* id = "literal_long_phrase";
  constexpr const char* phrase = "专属装置转速正常";
  const std::string entries =
      UserEntries() +
      "专属装置转速正常\tzhuān shǔ zhuāng zhì zhuǎn sù zhèng cháng\t5000\n";
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(id, entries, UserAlgebra()));
  const std::vector<std::pair<std::string, std::string>> choices = {
      {"zuan", "9826"},   {"shu", "748"},     {"zhuang", "948264"},
      {"zhi", "944"},     {"zuan", "9826"},   {"su", "78"},
      {"zheng", "94364"}, {"chang", "24264"},
  };
  std::string input;
  for (const auto& choice : choices)
    input += choice.second;
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(id));
  session.Type(input);
  EXPECT_EQ(1u, session.CandidateSet().count(phrase));
  for (const auto& choice : choices) {
    SCOPED_TRACE(choice.first);
    const auto tabs = FullSyllableTabs(session.Tabs());
    EXPECT_NE(tabs.end(), std::find(tabs.begin(), tabs.end(),
                                    choice.first + ':' +
                                        std::to_string(choice.second.size())));
    ASSERT_TRUE(session.SelectTab(choice.first.c_str(), choice.second.size()));
    EXPECT_EQ(1u, session.CandidateSet().count(phrase));
    EXPECT_EQ(input, session.Input());
  }
  for (auto choice = choices.rbegin(); choice != choices.rend(); ++choice) {
    SCOPED_TRACE("undo " + choice->first);
    ASSERT_TRUE(session.Backspace());
    EXPECT_EQ(input, session.Input());
    EXPECT_EQ(1u, session.CandidateSet().count(phrase));
    const auto tabs = FullSyllableTabs(session.Tabs());
    EXPECT_NE(tabs.end(), std::find(tabs.begin(), tabs.end(),
                                    choice->first + ':' +
                                        std::to_string(choice->second.size())));
  }
  session.Clear();
  EXPECT_TRUE(session.Input().empty());
  EXPECT_TRUE(session.Tabs().empty());
}

TEST(ScriptTranslatorLiteralTabRegression,
     UmlautAndAllToneSpellingsShareOneSelectableLabel) {
  constexpr const char* id = "literal_umlaut_tones";
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(
      id, "吕\tlü\t100\n驴\tlǖ\t100\n旅\tlǚ\t100\n绿\tlǜ\t100\n律\tlv\t100\n",
      std::string(kToneAlgebra) + kUppercaseT9Algebra));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(id));
  session.Type("58");
  EXPECT_EQ((std::vector<std::string>{"lv:2"}),
            FullSyllableTabs(session.Tabs()));
  ASSERT_TRUE(session.SelectTab("lv", 2));
  EXPECT_EQ((std::set<std::string>{"吕", "驴", "旅", "绿", "律"}),
            session.CandidateSet());
}

TEST(ScriptTranslatorLiteralTabRegression,
     ConfiguredDigitDelimiterIsNotCountedAsAPhoneticLetter) {
  constexpr const char* id = "literal_digit_delimiter";
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(
      id, kMinimalEntries, MinimalAlgebra(false), "1234567890", "1"));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(id));
  session.Type("9261");
  EXPECT_EQ((std::vector<std::string>{"wan:4", "yan:4", "zan:4"}),
            FullSyllableTabs(session.Tabs()));
  ASSERT_TRUE(session.SelectTab("zan", 4));
  EXPECT_EQ((std::set<std::string>{"赞", "站", "脏", "张"}),
            session.CandidateSet());
}

TEST(ScriptTranslatorLiteralTabRegression,
     LeadingDigitDelimiterKeepsLiteralLabelsAndSelection) {
  constexpr const char* id = "literal_leading_digit_delimiter";
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(
      id, kMinimalEntries, MinimalAlgebra(false), "1234567890", "1"));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(id));
  session.Type("1926");
  EXPECT_EQ((std::vector<std::string>{"wan:4", "yan:4", "zan:4"}),
            FullSyllableTabs(session.Tabs()));
  ASSERT_TRUE(session.SelectTab("zan", 4));
  EXPECT_EQ((std::set<std::string>{"赞", "站", "脏", "张"}),
            session.CandidateSet());
}

TEST(ScriptTranslatorLiteralTabRegression,
     AlphabeticCompletionWithDeriveHasNoTabs) {
  constexpr const char* id = "literal_alpha_completion_with_derive";
  ASSERT_NO_FATAL_FAILURE(BuildTabResources(
      id, "转\tzhuan\t100\n装\tzhuang\t100\n", "    - derive/^qqq$/rrr/\n",
      "abcdefghijklmnopqrstuvwxyz", " '", true));
  LiteralTabSession session;
  ASSERT_TRUE(session.Open(id));
  session.Type("zhua");
  // Letter-key input is exact, so it gets no syllable tabs.
  EXPECT_TRUE(session.Tabs().empty());
  EXPECT_EQ((std::set<std::string>{"转", "装"}), session.CandidateSet());
}

// Each row is an explicitly declared bidirectional phonetic class, independent
// of librime's projection implementation. Non-Mandarin derived spellings such
// as zuang are valid inputs but are not invented as dictionary syllables.
struct PhoneticClass {
  std::vector<std::string> syllables;
  std::vector<std::string> spellings;
};

std::vector<PhoneticClass> ExhaustiveClasses() {
  std::vector<PhoneticClass> classes = {
      {{"zan", "zang", "zhan", "zhang"}, {}},
      {{"can", "cang", "chan", "chang"}, {}},
      {{"san", "sang", "shan", "shang"}, {}},
      {{"zuan", "zhuan", "zhuang"}, {"zuan", "zuang", "zhuan", "zhuang"}},
      {{"cuan", "chuan", "chuang"}, {"cuan", "cuang", "chuan", "chuang"}},
      {{"suan", "shuan", "shuang"}, {"suan", "suang", "shuan", "shuang"}},
      {{"zen", "zeng", "zhen", "zheng"}, {}},
      {{"cen", "ceng", "chen", "cheng"}, {}},
      {{"sen", "seng", "shen", "sheng"}, {}},
      {{"bin", "bing"}, {}},
      {{"pin", "ping"}, {}},
      {{"min", "ming"}, {}},
      {{"nin", "ning"}, {}},
      {{"lin", "ling"}, {}},
      {{"jin", "jing"}, {}},
      {{"qin", "qing"}, {}},
      {{"xin", "xing"}, {}},
      {{"an", "ang"}, {}},
      {{"wan", "wang"}, {}},
      // The an/ang rules below expressly exclude these families. Keeping
      // their classes separate also tests same-key yuan/zuan and yan/zan.
      {{"xian"}, {}},
      {{"xiang"}, {}},
      {{"jian"}, {}},
      {{"jiang"}, {}},
      {{"qian"}, {}},
      {{"qiang"}, {}},
      {{"nian"}, {}},
      {{"niang"}, {}},
      {{"lian"}, {}},
      {{"liang"}, {}},
      {{"yan"}, {}},
      {{"yang"}, {}},
      {{"yuan"}, {}},
  };
  for (auto& phonetic_class : classes) {
    if (phonetic_class.spellings.empty())
      phonetic_class.spellings = phonetic_class.syllables;
  }
  return classes;
}

std::string ExhaustiveAlgebra(bool finals_first) {
  const std::string initials =
      "    - derive/^([zcs])h/$1/\n"
      "    - derive/^([zcs])([^h])/$1h$2/\n";
  // boost::regex<char> matches UTF-8 bytes. Whole-vowel alternatives keep
  // the i/u/y exclusion before the vowel, rather than letting [^iuy] consume
  // the first byte of a tone-marked vowel. The original user schema above is
  // retained verbatim; this independent fixture encodes its stated exclusions.
  const std::string finals =
      "    - derive/(ē|é|ě|è|e)ng$/$1n/\n"
      "    - derive/(ē|é|ě|è|e)n$/$1ng/\n"
      "    - derive/(ī|í|ǐ|ì|i)ng$/$1n/\n"
      "    - derive/(ī|í|ǐ|ì|i)n$/$1ng/\n"
      "    - derive/([^jqxy])u(ā|á|ǎ|à|a)ng$/$1u$2n/\n"
      "    - derive/([^jqxy])u(ā|á|ǎ|à|a)n$/$1u$2ng/\n"
      "    - derive/([^iuy])(ā|á|ǎ|à|a)ng$/$1$2n/\n"
      "    - derive/([^iuy])(ā|á|ǎ|à|a)n$/$1$2ng/\n"
      "    - derive/^(ā|á|ǎ|à|a)ng$/$1n/\n"
      "    - derive/^(ā|á|ǎ|à|a)n$/$1ng/\n";
  return (finals_first ? finals + initials : initials + finals) + kToneAlgebra +
         kUppercaseT9Algebra;
}

std::string T9Keys(const std::string& syllable) {
  constexpr const char* digits = "22233344455566677778889999";
  std::string result;
  for (const char letter : syllable) {
    EXPECT_GE(letter, 'a');
    EXPECT_LE(letter, 'z');
    if (letter < 'a' || letter > 'z')
      return {};
    result += digits[letter - 'a'];
  }
  return result;
}

std::string WithTone(const std::string& syllable, bool fourth) {
  // These fixtures use a/e/i finals; a has priority in ian/uan, so the
  // exclusion before a remains the literal i/u rather than a toned vowel.
  for (const auto& vowel : {std::make_pair('a', fourth ? "à" : "ā"),
                            std::make_pair('e', fourth ? "è" : "ē"),
                            std::make_pair('i', fourth ? "ì" : "ī")}) {
    const size_t position = syllable.find(vowel.first);
    if (position != std::string::npos) {
      std::string result = syllable;
      result.replace(position, 1, vowel.second);
      return result;
    }
  }
  ADD_FAILURE() << "Fixture has no supported tone vowel: " << syllable;
  return syllable;
}

void CheckAllLiteralSelections(const char* schema_id, bool finals_first) {
  const auto classes = ExhaustiveClasses();
  std::map<std::string, size_t> class_by_label;
  std::map<std::string, std::set<std::string>> labels_by_keys;
  std::vector<std::set<std::string>> expected_candidates(classes.size());
  std::set<std::string> inputs;
  std::string entries;
  size_t dictionary_entries = 0;
  for (size_t index = 0; index < classes.size(); ++index) {
    for (const auto& label : classes[index].syllables) {
      ASSERT_TRUE(class_by_label.emplace(label, index).second);
      labels_by_keys[T9Keys(label)].insert(label);
      const std::vector<std::string> original_spellings = {
          label, WithTone(label, false), WithTone(label, true)};
      for (size_t tone = 0; tone < original_spellings.size(); ++tone) {
        const std::string word = "t9_" + label + '_' + std::to_string(tone);
        entries += word + '\t' + original_spellings[tone] + "\t100\n";
        expected_candidates[index].insert(word);
        ++dictionary_entries;
      }
    }
    for (const auto& spelling : classes[index].spellings)
      inputs.insert(T9Keys(spelling));
  }
  ASSERT_NO_FATAL_FAILURE(
      BuildTabResources(schema_id, entries, ExhaustiveAlgebra(finals_first)));
  size_t selections = 0;
  for (const auto& input : inputs) {
    SCOPED_TRACE(std::string(schema_id) + " keys=" + input);
    std::vector<std::string> expected_tabs;
    for (const auto& label : labels_by_keys[input])
      expected_tabs.push_back(label + ':' + std::to_string(input.size()));
    {
      LiteralTabSession session;
      ASSERT_TRUE(session.Open(schema_id));
      session.Type(input);
      EXPECT_EQ(expected_tabs, FullSyllableTabs(session.Tabs()));
    }
    for (const auto& label : labels_by_keys[input]) {
      SCOPED_TRACE("select " + label);
      LiteralTabSession session;
      ASSERT_TRUE(session.Open(schema_id));
      session.Type(input);
      ASSERT_TRUE(session.SelectTab(label.c_str(), input.size()));
      EXPECT_EQ(expected_candidates[class_by_label.at(label)],
                session.CandidateSet());
      ++selections;
    }
  }
  EXPECT_EQ(class_by_label.size(), selections);
  testing::Test::RecordProperty("literal_labels", static_cast<int>(selections));
  testing::Test::RecordProperty("numeric_inputs",
                                static_cast<int>(inputs.size()));
  testing::Test::RecordProperty("dictionary_entries",
                                static_cast<int>(dictionary_entries));
  std::cout << "T9 exhaustive " << schema_id << ": " << inputs.size()
            << " numeric inputs, " << selections << " literal selections, "
            << dictionary_entries << " dictionary entries\n";
}

TEST(ScriptTranslatorLiteralTabExhaustive, InitialsThenFinals) {
  CheckAllLiteralSelections("literal_exhaustive_initials_first", false);
}

TEST(ScriptTranslatorLiteralTabExhaustive, FinalsThenInitials) {
  CheckAllLiteralSelections("literal_exhaustive_finals_first", true);
}

}  // namespace
