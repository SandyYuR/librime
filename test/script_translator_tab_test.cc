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

// Mixed schema: letters and digits share one alphabet, so a composition can
// hold a letter syllable before or after a digit one. Tab collection has to
// look at the syllable at the queried position instead of the first character
// of the whole input.
constexpr const char* kMixedSchema = R"YAML(# encoding: utf-8
schema:
  schema_id: tab_mixed_test
  name: tab mixed test
  version: "1"
engine:
  processors: [speller, fluency_editor]
  segmentors: [abc_segmentor]
  translators: [script_translator]
speller:
  alphabet: "abcdefghijklmnopqrstuvwxyz1234567890"
  delimiter: " '"
  algebra:
    - derive/^(.*)$/\U$1/
    - xlit/ABCDEFGHIJKLMNOPQRSTUVWXYZ/22233344455566677778889999/
translator:
  dictionary: dictionary_tab_mixed_test
  enable_completion: true
  enable_correction: false
  enable_user_dict: false
menu:
  page_size: 20
)YAML";

// Same dictionary, but a letter that is also in the alphabet is configured as
// the delimiter. A leading delimiter makes the query position point at a
// non-digit character, so tab collection has to skip it before judging whether
// the syllable uses digit keys.
constexpr const char* kLetterDelimiterSchema = R"YAML(# encoding: utf-8
schema:
  schema_id: tab_letter_delimiter_test
  name: tab letter delimiter test
  version: "1"
engine:
  processors: [speller, fluency_editor]
  segmentors: [abc_segmentor]
  translators: [script_translator]
speller:
  alphabet: "abcdefghijklmnopqrstuvwxyz1234567890"
  delimiter: "a"
  algebra:
    - derive/^(.*)$/\U$1/
    - xlit/ABCDEFGHIJKLMNOPQRSTUVWXYZ/22233344455566677778889999/
translator:
  dictionary: dictionary_tab_mixed_test
  enable_completion: true
  enable_correction: false
  enable_user_dict: false
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
void BuildDictionary(const char* schema_file,
                     const std::string& name = "dictionary_tab_test") {
  rime::the<rime::Dictionary> dict(new rime::Dictionary(
      name, {}, {rime::New<rime::Table>(rime::path{name + ".table.bin"})},
      rime::New<rime::Prism>(rime::path{name + ".prism.bin"})));
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

  bool SelectCandidate(const std::string& text) {
    int index = -1;
    RIME_STRUCT(RimeContext, ctx);
    if (api_->get_context(session_, &ctx)) {
      for (int i = 0; i < ctx.menu.num_candidates; ++i) {
        if (ctx.menu.candidates[i].text &&
            text == ctx.menu.candidates[i].text) {
          index = i;
          break;
        }
      }
      api_->free_context(&ctx);
    }
    return index >= 0 && api_->select_candidate(session_, index);
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

std::string SelectWithAlgebra(const char* schema_id,
                              const char* algebra,
                              const std::string& keys,
                              const char* label,
                              size_t span,
                              const char* speller_options = "",
                              const char* translator_options = "") {
  const std::string schema_file = std::string(schema_id) + ".schema.yaml";
  {
    std::ofstream schema(schema_file);
    schema
        << "schema:\n  schema_id: " << schema_id
        << "\n  name: ordered tab test\n  version: '1'\n"
           "engine:\n  processors: [speller]\n"
           "  segmentors: [abc_segmentor]\n"
           "  translators: [script_translator]\n"
           "speller:\n  alphabet: '1234567890'\n"
        << speller_options << "  algebra:\n"
        << algebra
        << "    - xlit/abcdefghijklmnopqrstuvwxyz/22233344455566677778889999/\n"
           "translator:\n  dictionary: dictionary_tab_ordered_test\n"
        << translator_options << "menu:\n  page_size: 20\n";
  }
  BuildDictionary(schema_file.c_str(), "dictionary_tab_ordered_test");
  Session session;
  session.Open(schema_id);
  session.Type(keys);
  EXPECT_TRUE(session.SelectTab(0, label, span)) << Describe(session.Tabs());
  return session.Candidates();
}

}  // namespace

TEST(ScriptTranslatorTab, OrdersSyllableTabsLongestFirstThenAlphabetically) {
  std::ofstream("tab_fuzzy_test.schema.yaml") << kFuzzySchema;
  BuildDictionary("tab_fuzzy_test.schema.yaml");

  Session session;
  session.Open("tab_fuzzy_test");
  session.Type(kSheng);

  auto tabs = session.Tabs();
  ASSERT_FALSE(tabs.empty());
  // Literal five-key syllables first, then literal three-key syllables in
  // alphabetical order. The fuzzy-only "shen" label needs four keys and is
  // not shown for the five-key edge. Initial/raw-key tabs stay at the end.
  EXPECT_EQ("sheng:5|pie:3|qie:3|she:3|p:1|q:1|r:1|s:1|7:1", Describe(tabs));
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
  // The tab bar only offers what was typed: "sheng" needs five keys, so it is
  // not offered for 7436 even though the fuzzy spelling "shen" reaches it.
  EXPECT_EQ("shen:4|p:1|q:1|r:1|s:1|7:1", Describe(session.Tabs()));

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

// Letter-key (26-key) input is exact, so it gets no syllable tabs at all: the
// bar only helps digit-key input, where several syllables share one key.  The
// fuzzy spelling still takes part in translation, so "shen" keeps 生 although
// its canonical spelling is "sheng".
TEST(ScriptTranslatorTab, LetterInputHasNoTabs) {
  std::ofstream("tab_letter_test.schema.yaml") << R"YAML(# encoding: utf-8
schema:
  schema_id: tab_letter_test
  name: tab letter test
  version: "1"
engine:
  processors: [speller]
  segmentors: [abc_segmentor]
  translators: [script_translator]
speller:
  alphabet: "abcdefghijklmnopqrstuvwxyz"
  algebra:
    - derive/(e)ng(.*)$/$1n$2/
    - derive/(e)n([^g].*)?$/$1ng$2/
translator:
  dictionary: dictionary_tab_test
menu:
  page_size: 20
)YAML";
  BuildDictionary("tab_letter_test.schema.yaml");

  Session session;
  session.Open("tab_letter_test");
  session.Type("shen");

  // No syllable tabs on letter input, not even the fuzzy-only "sheng" label.
  EXPECT_TRUE(session.Tabs().empty()) << Describe(session.Tabs());
  EXPECT_EQ("shen", session.Preedit());

  std::string candidates = session.Candidates();
  EXPECT_NE(std::string::npos, candidates.find("沈")) << candidates;
  EXPECT_NE(std::string::npos, candidates.find("生")) << candidates;
}

TEST(ScriptTranslatorTab, TransformationBeforeDerivationDoesNotInventRelation) {
  const auto candidates = SelectWithAlgebra(
      "tab_prior_xform", "    - xform/^qi$/ri/\n    - derive/^qi$/pi/\n", kPi,
      "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("皮")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("七")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("日")) << candidates;
}

TEST(ScriptTranslatorTab, DerivationUsesTransformedSpelling) {
  const auto candidates = SelectWithAlgebra(
      "tab_normalized_derive", "    - xform/^qi$/ri/\n    - derive/^ri$/pi/\n",
      kPi, "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("皮")) << candidates;
  EXPECT_NE(std::string::npos, candidates.find("七")) << candidates;
  EXPECT_NE(std::string::npos, candidates.find("日")) << candidates;
}

TEST(ScriptTranslatorTab, DerivedRelationSurvivesFinalKeyCollision) {
  const auto candidates = SelectWithAlgebra(
      "tab_derived_collision", "    - derive/^qi$/pi/\n", kPi, "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("七")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("日")) << candidates;
}

TEST(ScriptTranslatorTab, FuzzRelationSurvivesFinalKeyCollision) {
  const auto candidates = SelectWithAlgebra(
      "tab_fuzz_collision", "    - fuzz/^qi$/pi/\n", kPi, "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("七")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("日")) << candidates;
}

TEST(ScriptTranslatorTab, DerivationUsesOriginalToneMarkedSpelling) {
  const auto candidates = SelectWithAlgebra(
      "tab_toned_derive", "    - derive/^shēng$/shēn/\n    - xform/ē/e/\n",
      kShen, "shen", 4);
  EXPECT_NE(std::string::npos, candidates.find("甥")) << candidates;
}

TEST(ScriptTranslatorTab, ErasedDerivedBranchDoesNotLeaveRelation) {
  const auto candidates =
      SelectWithAlgebra("tab_erased_derive",
                        "    - xform/^pi$/pI/\n    - derive/^qi$/pi/\n"
                        "    - erase/^pi$/\n    - xform/^pI$/pi/\n",
                        kPi, "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("皮")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("七")) << candidates;
}

TEST(ScriptTranslatorTab, DerivedBranchMustReachTheSelectedInputEdge) {
  const auto candidates =
      SelectWithAlgebra("tab_other_derived_key",
                        "    - xform/^pi$/pI/\n    - derive/^qi$/pi/\n"
                        "    - xform/^pi$/bi/\n    - xform/^pI$/pi/\n",
                        kPi, "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("皮")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("七")) << candidates;
}

TEST(ScriptTranslatorTab, DerivationFollowedByNormalizationKeepsRelation) {
  const auto candidates = SelectWithAlgebra(
      "tab_normalized_variant", "    - derive/^qi$/zz/\n    - xform/^zz$/pi/\n",
      kPi, "pi", 2);
  EXPECT_NE(std::string::npos, candidates.find("七")) << candidates;
}

TEST(ScriptTranslatorTab, TransformationAloneDoesNotBecomeFuzzy) {
  const auto candidates = SelectWithAlgebra(
      "tab_xform_only_relation", "    - derive/^qi$/pi/\n    - xform/^n$/en/\n",
      "36", "en", 2);
  EXPECT_NE(std::string::npos, candidates.find("恩")) << candidates;
  EXPECT_EQ(std::string::npos, candidates.find("嗯")) << candidates;
}

TEST(ScriptTranslatorTab, DerivedRelationMatchesEdgeWithTrailingDelimiter) {
  const auto candidates =
      SelectWithAlgebra("tab_derived_delimiter", "    - derive/^sheng$/shen/\n",
                        "7436'", "shen", 5, "  delimiter: \"'\"\n");
  EXPECT_NE(std::string::npos, candidates.find("生")) << candidates;
}

TEST(ScriptTranslatorTab, DerivedRelationMatchesCanonicalizedInput) {
  const auto candidates = SelectWithAlgebra(
      "tab_derived_canonical", "    - derive/^sheng$/shen/\n", "8436", "shen",
      4, "", "  canonicalize:\n    - xform/8/7/\n");
  EXPECT_NE(std::string::npos, candidates.find("生")) << candidates;
}

TEST(ScriptTranslatorTab, DerivedRelationMatchesCompletionEdge) {
  const auto candidates =
      SelectWithAlgebra("tab_derived_completion",
                        "    - derive/^sheng$/shen/\n    - xform/^shen$/aabb/\n"
                        "    - xform/^sheng$/aabbb/\n",
                        "222", "shen", 3, "", "  enable_completion: true\n");
  EXPECT_NE(std::string::npos, candidates.find("生")) << candidates;
}

TEST(ScriptTranslatorTab, MixedInputShowsTabsOnlyForDigitSyllables) {
  std::ofstream("tab_mixed_test.schema.yaml") << kMixedSchema;
  BuildDictionary("tab_mixed_test.schema.yaml", "dictionary_tab_mixed_test");

  Session session;
  session.Open("tab_mixed_test");
  session.Type("shen9826");
  // Position 0 is a letter syllable: its keys are exact, so the bar is hidden.
  EXPECT_TRUE(session.Tabs(0).empty());
  // Position 4 starts the digit syllable 9826, which does need disambiguation.
  const auto digit_tabs = session.Tabs(4);
  ASSERT_FALSE(digit_tabs.empty());
  EXPECT_NE(std::string::npos, Describe(digit_tabs).find("zuan:4"))
      << Describe(digit_tabs);
}

TEST(ScriptTranslatorTab, LetterSyllableHasNoTabsAfterDigitPrefix) {
  std::ofstream("tab_mixed_test.schema.yaml") << kMixedSchema;
  BuildDictionary("tab_mixed_test.schema.yaml", "dictionary_tab_mixed_test");

  Session session;
  session.Open("tab_mixed_test");
  session.Type("7436zhua");
  // The digit prefix still offers syllable tabs; the letter syllable after it
  // stays exact and therefore shows no tabs of its own.
  const auto prefix_tabs = session.Tabs(0);
  ASSERT_FALSE(prefix_tabs.empty());
  EXPECT_NE(std::string::npos, Describe(prefix_tabs).find("shen:4"))
      << Describe(prefix_tabs);
  EXPECT_TRUE(session.Tabs(4).empty());
}

TEST(ScriptTranslatorTab, ConfirmedPrefixKeepsFollowingSyllableKind) {
  std::ofstream("tab_mixed_test.schema.yaml") << kMixedSchema;
  BuildDictionary("tab_mixed_test.schema.yaml", "dictionary_tab_mixed_test");

  Session letter_first;
  letter_first.Open("tab_mixed_test");
  letter_first.Type("shen9826");
  // Confirm the letter prefix by really choosing a candidate. Selecting the
  // tab "沈"/4 would only move the tab cursor and empty the menu, so these
  // assertions could pass without a confirmed prefix.
  ASSERT_TRUE(letter_first.SelectCandidate("沈"));
  const std::string letter_preedit = letter_first.Preedit();
  EXPECT_EQ(std::string::npos, letter_preedit.find("shen")) << letter_preedit;
  EXPECT_NE(std::string::npos, letter_preedit.find("9826")) << letter_preedit;
  EXPECT_NE(std::string::npos, letter_first.Candidates().find("钻"))
      << letter_first.Candidates();
  // The cursor moves past the confirmed letter prefix, so the automatic
  // position now points at the digit syllable and tabs must reappear.
  const auto digit_tabs = letter_first.Tabs();
  ASSERT_FALSE(digit_tabs.empty());
  EXPECT_NE(std::string::npos, Describe(digit_tabs).find("zuan:4"))
      << Describe(digit_tabs);

  Session digit_first;
  digit_first.Open("tab_mixed_test");
  digit_first.Type("7436zhua");
  ASSERT_TRUE(digit_first.SelectCandidate("沈"));
  const std::string digit_preedit = digit_first.Preedit();
  EXPECT_EQ(std::string::npos, digit_preedit.find("7436")) << digit_preedit;
  EXPECT_NE(std::string::npos, digit_preedit.find("zhua")) << digit_preedit;
  EXPECT_NE(std::string::npos, digit_first.Candidates().find("转"))
      << digit_first.Candidates();
  // Same cursor advance in the other direction: the remaining letters are
  // exact, so the bar disappears even though the input starts with digits.
  EXPECT_TRUE(digit_first.Tabs().empty());
}

TEST(ScriptTranslatorTab, DelimiterDoesNotHideDigitSyllableTabs) {
  std::ofstream("tab_mixed_test.schema.yaml") << kMixedSchema;
  BuildDictionary("tab_mixed_test.schema.yaml", "dictionary_tab_mixed_test");

  Session session;
  session.Open("tab_mixed_test");
  session.Type("7436'zhua");
  // Digits before the configured delimiter keep their syllable tabs.
  const auto digit_tabs = session.Tabs(0);
  ASSERT_FALSE(digit_tabs.empty());
  EXPECT_NE(std::string::npos, Describe(digit_tabs).find("shen:"))
      << Describe(digit_tabs);
  // The letters behind the delimiter stay exact, so their position has none.
  EXPECT_TRUE(session.Tabs(5).empty());
}

TEST(ScriptTranslatorTab, LeadingLetterDelimiterDoesNotHideDigitSyllableTabs) {
  std::ofstream("tab_letter_delimiter_test.schema.yaml")
      << kLetterDelimiterSchema;
  BuildDictionary("tab_letter_delimiter_test.schema.yaml",
                  "dictionary_tab_mixed_test");

  Session session;
  session.Open("tab_letter_delimiter_test");
  session.Type("a9826");
  // Position 0 points at the configured delimiter, not at the digit syllable;
  // collection has to skip it before deciding whether tabs are offered. The
  // span covers the delimiter, hence zuan:5.
  const auto tabs = session.Tabs(0);
  ASSERT_FALSE(tabs.empty());
  EXPECT_NE(std::string::npos, Describe(tabs).find("zuan:5")) << Describe(tabs);
}
