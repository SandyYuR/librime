//
// Copyright RIME Developers
// Distributed under the BSD License
//
// T9 (nine-key) input tabs:
//  * syllable tabs are ordered by span descending, then label ascending;
//  * selecting a syllable tab keeps syllables that the schema's fuzzy-spelling
//    algebra also spells as the selected label (en/eng fuzzy: tab "shen" must
//    keep "sheng"), while syllables that merely share the same key (tab "pi"
//    must not keep "qi"/"ri") are still dropped;
//  * single-letter abbreviation tabs and the raw-key tab stay at the end.
//
#include <gtest/gtest.h>
#include <rime_api.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/prism.h>
#include <rime/dict/table.h>
#include <fstream>
#include <string>
#include <vector>

namespace {

// T9 digits: 2=abc 3=def 4=ghi 5=jkl 6=mno 7=pqrs 8=tuv 9=wxyz.
// "sheng" -> 74364, "shenxiao" -> 7436 9426, "shen" -> 7436, "pi" -> 74.
constexpr const char* kSheng = "74364";
constexpr const char* kShenXiao = "74369426";
constexpr const char* kShen = "7436";
constexpr const char* kPi = "74";

// InputTabEntry::Source values, see rime/translator.h.
constexpr int kSyllableTab = 0;
constexpr int kRawInputTab = 3;

// Nine-key schema with the Wanxiang en/eng fuzzy-spelling rules: "sheng" is
// also indexed under the spelling "shen", so both syllables share the key 7436.
constexpr const char* kFuzzySchema = R"YAML(# encoding: utf-8
schema:
  schema_id: tab_fuzzy_test
  name: tab fuzzy test
  version: "1"
engine:
  processors: [speller]
  segmentors: [abc_segmentor]
  translators: [script_translator]
speller:
  alphabet: "1234567890"
  algebra:
    - derive/(e)ng(.*)$/$1n$2/
    - derive/(e)n([^g].*)?$/$1ng$2/
    - xlit/abcdefghijklmnopqrstuvwxyz/22233344455566677778889999/
translator:
  dictionary: dictionary_tab_test
menu:
  page_size: 20
)YAML";

// Same key mapping without any fuzzy-spelling rule: every syllable is indexed
// under its own spelling only, so tab selection must behave as before.
constexpr const char* kPlainSchema = R"YAML(# encoding: utf-8
schema:
  schema_id: tab_plain_test
  name: tab plain test
  version: "1"
engine:
  processors: [speller]
  segmentors: [abc_segmentor]
  translators: [script_translator]
speller:
  alphabet: "1234567890"
  algebra:
    - xlit/abcdefghijklmnopqrstuvwxyz/22233344455566677778889999/
translator:
  dictionary: dictionary_tab_test
menu:
  page_size: 20
)YAML";

struct Tab {
  std::string label;
  size_t span;
  int source;
};

// Compiles data/test/dictionary_tab_test.dict.yaml into the working directory,
// which the test environment uses as the deployed resource root.  The prism
// keys are projected through the given schema's spelling algebra, so the schema
// file has to exist before compiling.
void BuildDictionary(const char* schema_file) {
  rime::the<rime::Dictionary> dict(new rime::Dictionary(
      "dictionary_tab_test", {},
      {rime::New<rime::Table>(rime::path{"dictionary_tab_test.table.bin"})},
      rime::New<rime::Prism>(rime::path{"dictionary_tab_test.prism.bin"})));
  dict->Remove();
  rime::DictCompiler compiler(dict.get());
  ASSERT_TRUE(compiler.Compile(rime::path{schema_file}));
  ASSERT_TRUE(dict->Load());
}

// "label:span" joined by '|', for readable assertions on the whole tab bar.
std::string Describe(const std::vector<Tab>& tabs) {
  std::string result;
  for (const auto& tab : tabs) {
    if (!result.empty())
      result += '|';
    result += tab.label + ":" + std::to_string(tab.span);
  }
  return result;
}

class Session {
 public:
  Session() : api_(rime_get_api()) {}
  ~Session() {
    if (session_)
      api_->destroy_session(session_);
  }

  void Open(const char* schema_id) {
    session_ = api_->create_session();
    ASSERT_NE(0, session_);
    ASSERT_TRUE(api_->select_schema(session_, schema_id));
  }

  void Type(const std::string& keys) {
    for (char key : keys)
      api_->process_key(session_, static_cast<unsigned char>(key), 0);
  }

  std::vector<Tab> Tabs(size_t position = 0) {
    std::vector<Tab> tabs;
    char** labels = nullptr;
    size_t* spans = nullptr;
    int* sources = nullptr;
    size_t count = 0;
    if (!api_->get_input_tabs(session_, position, &labels, &spans, &sources,
                              &count))
      return tabs;
    for (size_t i = 0; i < count; ++i)
      tabs.push_back(Tab{labels[i] ? labels[i] : "", spans[i], sources[i]});
    api_->free_input_tabs(labels, spans, sources, count);
    return tabs;
  }

  bool SelectTab(size_t position, const std::string& label, size_t span) {
    return api_->select_tab(session_, position, label.c_str(), span);
  }

  std::string Preedit() {
    std::string result;
    RIME_STRUCT(RimeContext, ctx);
    if (api_->get_context(session_, &ctx)) {
      if (ctx.composition.preedit)
        result = ctx.composition.preedit;
      api_->free_context(&ctx);
    }
    return result;
  }

  std::string Candidates() {
    std::string result;
    RIME_STRUCT(RimeContext, ctx);
    if (api_->get_context(session_, &ctx)) {
      for (int i = 0; i < ctx.menu.num_candidates; ++i) {
        if (i)
          result += '|';
        result +=
            ctx.menu.candidates[i].text ? ctx.menu.candidates[i].text : "";
      }
      api_->free_context(&ctx);
    }
    return result;
  }

 private:
  RimeApi* api_;
  RimeSessionId session_ = 0;
};

}  // namespace

TEST(ScriptTranslatorTab, OrdersSyllableTabsLongestFirstThenAlphabetically) {
  std::ofstream("tab_fuzzy_test.schema.yaml") << kFuzzySchema;
  BuildDictionary("tab_fuzzy_test.schema.yaml");

  Session session;
  session.Open("tab_fuzzy_test");
  session.Type(kSheng);

  auto tabs = session.Tabs();
  ASSERT_FALSE(tabs.empty());
  // Five-key syllables first ("shen" also covers 74364 through the fuzzy
  // spelling "sheng"), then the three-key syllables, and alphabetical within a
  // span.  The xlit single-letter tabs and the raw key tab stay at the end.
  EXPECT_EQ("shen:5|sheng:5|pie:3|qie:3|she:3|p:1|q:1|r:1|s:1|7:1",
            Describe(tabs));
  for (const auto& tab : tabs) {
    if (tab.span == 1)
      break;
    EXPECT_EQ(kSyllableTab, tab.source);
  }
  EXPECT_EQ(kRawInputTab, tabs.back().source);
  EXPECT_EQ("7", tabs.back().label);
}

TEST(ScriptTranslatorTab, FuzzyTabSelectionKeepsDerivedSyllable) {
  std::ofstream("tab_fuzzy_test.schema.yaml") << kFuzzySchema;
  BuildDictionary("tab_fuzzy_test.schema.yaml");

  Session session;
  session.Open("tab_fuzzy_test");
  session.Type(kShen);

  // "sheng" is reachable through the fuzzy spelling "shen" on key 7436.
  EXPECT_EQ("shen:4|sheng:4|p:1|q:1|r:1|s:1|7:1", Describe(session.Tabs()));

  ASSERT_TRUE(session.SelectTab(0, "shen", 4));
  std::string candidates = session.Candidates();
  EXPECT_NE(std::string::npos, candidates.find("沈")) << candidates;
  EXPECT_NE(std::string::npos, candidates.find("生")) << candidates;
}

TEST(ScriptTranslatorTab, FuzzyTabSelectionKeepsWholeSentence) {
  std::ofstream("tab_fuzzy_test.schema.yaml") << kFuzzySchema;
  BuildDictionary("tab_fuzzy_test.schema.yaml");

  Session session;
  session.Open("tab_fuzzy_test");
  session.Type(kShenXiao);

  // Without a selection "shenxiao" already yields 生效: "sheng" is reachable
  // through the fuzzy spelling "shen" on the first four keys.
  EXPECT_NE(std::string::npos, session.Candidates().find("生效"));

  // Selecting the shorter "shen" tab must not drop the fuzzy "sheng" path.
  ASSERT_TRUE(session.SelectTab(0, "shen", 4));
  std::string candidates = session.Candidates();
  EXPECT_NE(std::string::npos, candidates.find("生")) << candidates;
  EXPECT_NE(std::string::npos, candidates.find("生效")) << candidates;
}

TEST(ScriptTranslatorTab, SelectingTabDropsSameKeySyllables) {
  std::ofstream("tab_fuzzy_test.schema.yaml") << kFuzzySchema;
  BuildDictionary("tab_fuzzy_test.schema.yaml");

  Session session;
  session.Open("tab_fuzzy_test");
  session.Type(kPi);

  EXPECT_EQ("pi:2|qi:2|ri:2|p:1|q:1|r:1|s:1|7:1", Describe(session.Tabs()));

  // "pi", "qi" and "ri" share the key 74 but are not fuzzy variants of each
  // other, so selecting "pi" keeps only "pi".
  ASSERT_TRUE(session.SelectTab(0, "pi", 2));
  std::string candidates = session.Candidates();
  EXPECT_NE(std::string::npos, candidates.find("皮")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("七")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("日")) << candidates;
}

TEST(ScriptTranslatorTab, SingleLetterAndRawTabsStayAtTheEnd) {
  std::ofstream("tab_fuzzy_test.schema.yaml") << kFuzzySchema;
  BuildDictionary("tab_fuzzy_test.schema.yaml");

  Session session;
  session.Open("tab_fuzzy_test");
  session.Type(kShenXiao);

  auto tabs = session.Tabs();
  ASSERT_GE(tabs.size(), 5u);
  // 简拼: one tab per letter mapped to the first key, then the raw key.
  std::vector<std::string> tail;
  for (size_t i = tabs.size() - 5; i < tabs.size(); ++i)
    tail.push_back(tabs[i].label);
  EXPECT_EQ((std::vector<std::string>{"p", "q", "r", "s", "7"}), tail);

  for (size_t i = tabs.size() - 5; i + 1 < tabs.size(); ++i)
    EXPECT_EQ(kSyllableTab, tabs[i].source);
  EXPECT_EQ(kRawInputTab, tabs.back().source);

  // Selecting an abbreviation tab records the constraint and leaves the
  // composition untouched.  On all-digit T9 input the shadowed single-letter
  // label is not part of the speller alphabet, so the digit segment is not
  // translated and the menu stays empty; that is pre-existing behaviour of the
  // digit-input path and is not affected by the syllable-tab changes.
  ASSERT_TRUE(session.SelectTab(0, "s", 1));
  EXPECT_EQ(kShenXiao, session.Preedit());
  EXPECT_FALSE(session.Tabs().empty());
}

TEST(ScriptTranslatorTab, SchemaWithoutFuzzyRulesIsUnchanged) {
  std::ofstream("tab_plain_test.schema.yaml") << kPlainSchema;
  BuildDictionary("tab_plain_test.schema.yaml");

  Session session;
  session.Open("tab_plain_test");
  session.Type(kShen);

  // Without en/eng fuzzy spelling "sheng" is not indexed under 7436, so the
  // tab bar only offers "shen".
  EXPECT_EQ("shen:4|p:1|q:1|r:1|s:1|7:1", Describe(session.Tabs()));

  ASSERT_TRUE(session.SelectTab(0, "shen", 4));
  std::string candidates = session.Candidates();
  EXPECT_NE(std::string::npos, candidates.find("沈")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("生")) << candidates;
}
