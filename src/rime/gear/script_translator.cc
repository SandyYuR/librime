//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Script translator
//
// 2011-07-10 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <stack>
#include <cmath>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/translation.h>
#include <rime/algo/algebra.h>
#include <rime/algo/calculus.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/corrector.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/poet.h>
#include <rime/gear/script_translator.h>
#include <rime/gear/translator_commons.h>

// static const char* quote_left = "\xef\xbc\x88";
// static const char* quote_right = "\xef\xbc\x89";

namespace rime {

namespace {

struct SyllabifyTask {
  const Code& code;
  const SyllableGraph& graph;
  size_t target_pos;
  function<void(SyllabifyTask* task,
                size_t depth,
                size_t current_pos,
                size_t next_pos)>
      push;
  function<void(SyllabifyTask* task, size_t depth)> pop;
};

static bool syllabify_dfs(SyllabifyTask* task,
                          size_t depth,
                          size_t current_pos) {
  if (depth == task->code.size()) {
    return current_pos == task->target_pos;
  }
  SyllableId syllable_id = task->code.at(depth);
  auto z = task->graph.edges.find(current_pos);
  if (z == task->graph.edges.end())
    return false;
  // favor longer spellings
  for (const auto& y : boost::adaptors::reverse(z->second)) {
    size_t end_vertex_pos = y.first;
    if (end_vertex_pos > task->target_pos)
      continue;
    auto x = y.second.find(syllable_id);
    if (x != y.second.end()) {
      task->push(task, depth, current_pos, end_vertex_pos);
      if (syllabify_dfs(task, depth + 1, end_vertex_pos))
        return true;
      task->pop(task, depth);
    }
  }
  return false;
}

}  // anonymous namespace

static string NormalizeTabLabel(const string& label);
static string NormalizeT9TabLabel(const string& label);

// Replay the deployed algebra while retaining the labels reached by a fuzzy
// branch. Final keys can collide (e.g. qi/pi -> 74), so a set of final keys
// alone cannot establish which syllable label a branch actually derives.
class FuzzySpellingProjection : public Projection {
 public:
  bool Load(an<ConfigList> rules) {
    fuzzy_rules_.clear();
    cache_.clear();
    literal_cache_.clear();
    if (!Projection::Load(rules))
      return false;
    bool has_fuzzy_rule = false;
    for (size_t i = 0; i < rules->size(); ++i) {
      const string& formula = rules->GetValueAt(i)->str();
      const size_t separator =
          formula.find_first_not_of("abcdefghijklmnopqrstuvwxyz");
      const string operation = formula.substr(0, separator);
      const bool fuzzy = operation == "derive" || operation == "fuzz";
      fuzzy_rules_.push_back(fuzzy);
      has_fuzzy_rule |= fuzzy;
    }
    return has_fuzzy_rule;
  }

  // True when the keys of the input edge spell `syllable` itself, that is, the
  // prism indexes it under its own spelling for that edge, and not under a
  // phonetic variant, abbreviation or completion. Keep ordinary transforms
  // and spelling-preserving encoding branches, such as Wanxiang's lowercase
  // -> uppercase derive followed by uppercase -> digit xlit. A phonetic
  // derive such as zuan -> zhuan must not supply a literal tab.
  bool SpellsLiterally(const string& syllable,
                       const set<int>& matched_spellings,
                       Prism& prism) {
    auto cached = literal_cache_.find(syllable);
    if (cached == literal_cache_.end()) {
      set<string> keys{syllable};
      for (auto& calculation : calculation_) {
        set<string> next;
        for (const auto& key : keys) {
          Spelling spelling(key);
          bool applied;
          try {
            applied = calculation->Apply(&spelling);
          } catch (const std::runtime_error& error) {
            LOG(ERROR) << "Error applying tab spelling algebra: "
                       << error.what();
            return false;
          }
          if (!applied || !calculation->deletion())
            next.insert(key);
          if (applied && calculation->addition() && !spelling.str.empty() &&
              (calculation->deletion() ||
               (spelling.properties.type == kNormalSpelling &&
                !spelling.properties.is_correction &&
                NormalizeT9TabLabel(key) ==
                    NormalizeT9TabLabel(spelling.str)))) {
            next.insert(spelling.str);
          }
        }
        keys.swap(next);
      }
      cached = literal_cache_.emplace(syllable, std::move(keys)).first;
    }
    for (const auto& key : cached->second) {
      int spelling_id;
      if (prism.GetValue(key, &spelling_id) &&
          matched_spellings.count(spelling_id))
        return true;
    }
    return false;
  }

  bool Matches(const string& syllable,
               const string& label,
               const set<int>& matched_spellings,
               Prism& prism,
               bool normalize_t9) {
    auto cached = cache_.find(syllable);
    if (cached == cache_.end()) {
      Paths paths;
      paths[syllable];
      for (size_t i = 0; i < calculation_.size(); ++i) {
        auto& calculation = calculation_[i];
        Paths next;
        for (const auto& [key, labels] : paths) {
          Spelling spelling(key);
          bool applied;
          try {
            applied = calculation->Apply(&spelling);
          } catch (const std::runtime_error& error) {
            LOG(ERROR) << "Error applying tab spelling algebra: "
                       << error.what();
            return false;
          }
          if (!applied || !calculation->deletion())
            Merge(&next, key, labels);
          if (applied && calculation->addition() && !spelling.str.empty()) {
            auto derived_labels = labels;
            if (fuzzy_rules_[i] || !labels.empty())
              derived_labels.insert(StripTones(spelling.str));
            Merge(&next, spelling.str, derived_labels);
          }
        }
        paths.swap(next);
      }
      cached = cache_.emplace(syllable, std::move(paths)).first;
    }
    for (const auto& [key, labels] : cached->second) {
      const bool matches_label =
          labels.count(label) ||
          (normalize_t9 &&
           std::any_of(labels.begin(), labels.end(), [&](const string& value) {
             return NormalizeT9TabLabel(value) == label;
           }));
      int spelling_id;
      if (matches_label && prism.GetValue(key, &spelling_id) &&
          matched_spellings.count(spelling_id))
        return true;
    }
    return false;
  }

 private:
  using Paths = map<string, set<string>>;

  static void Merge(Paths* paths,
                    const string& key,
                    const set<string>& labels) {
    auto& target = (*paths)[key];
    target.insert(labels.begin(), labels.end());
  }

  vector<bool> fuzzy_rules_;
  map<string, Paths> cache_;
  map<string, set<string>> literal_cache_;
};

// Use the same prism searches as the graph builder when identifying the keys
// for a selected edge, including delimiters, canonicalization and completion.
class TabSyllabifier : public Syllabifier {
 public:
  using Syllabifier::Syllabifier;

  set<int> MatchSpellings(const string& input,
                          size_t start,
                          size_t end,
                          Prism& prism,
                          bool completion,
                          bool correction = true) {
    set<int> result;
    vector<Prism::Match> matches;
    if (completion) {
      if (enable_completion_ && end == input.length()) {
        prism.ExpandSearch(input.substr(start), &matches, 512);
        for (const auto& match : matches) {
          if (match.length >= end - start)
            result.insert(match.value);
        }
      }
      return result;
    }
    size_t begin = start;
    while (begin < input.length() &&
           delimiters_.find(input[begin]) != string::npos)
      ++begin;
    const string remaining = input.substr(begin);
    if (canonicalizer_) {
      const size_t limit =
          (std::min)(remaining.length(), prism.max_key_length());
      for (size_t length = 1; length <= limit; ++length) {
        string key = remaining.substr(0, length);
        canonicalizer_->Apply(&key);
        int value;
        if (prism.GetValue(key, &value))
          matches.push_back({value, length});
      }
    } else {
      prism.CommonPrefixSearch(remaining, &matches);
    }
    if (correction && corrector_) {
      corrector::Corrections corrections;
      corrector_->ToleranceSearch(prism, remaining, &corrections, 5);
      for (const auto& [value, correction] : corrections) {
        for (auto accessor = prism.QuerySpelling(value); !accessor.exhausted();
             accessor.Next()) {
          const auto properties = accessor.properties();
          if (properties.type == kNormalSpelling && !properties.is_correction) {
            matches.push_back({value, correction.length});
            break;
          }
        }
      }
    }
    for (const auto& match : matches) {
      size_t matched_end = begin + match.length;
      while (matched_end < input.length() &&
             delimiters_.find(input[matched_end]) != string::npos)
        ++matched_end;
      if (match.length && matched_end == end)
        result.insert(match.value);
    }
    return result;
  }
};

class ScriptSyllabifier : public PhraseSyllabifier {
 public:
  ScriptSyllabifier(ScriptTranslator* translator,
                    Corrector* corrector,
                    const string& input,
                    size_t start)
      : translator_(translator),
        input_(input),
        start_(start),
        syllabifier_(translator->delimiters(),
                     translator->enable_completion(),
                     translator->strict_spelling(),
                     translator->canonicalizer()) {
    if (corrector) {
      syllabifier_.EnableCorrection(corrector);
    }
  }

  virtual Spans Syllabify(const Phrase* phrase);
  size_t BuildSyllableGraph(Prism& prism);
  void ApplyTabConstraints(Dictionary* dict,
                           const vector<Context::TabConstraint>& constraints);
  string GetPreeditString(const Phrase& cand) const;
  string GetOriginalSpelling(const Phrase& cand) const;
  bool IsCorrection(const Code& code, size_t code_length) const;

  const SyllableGraph& syllable_graph() const { return syllable_graph_; }

  // Prism spellings that match input_[begin, end) exactly, as the syllable
  // graph builder sees them.
  set<int> MatchLiteralSpellings(size_t begin, size_t end, Prism& prism) {
    return syllabifier_.MatchSpellings(input_, begin, end, prism, false, false);
  }

 protected:
  ScriptTranslator* translator_;
  string input_;
  size_t start_;
  TabSyllabifier syllabifier_;
  SyllableGraph syllable_graph_;
};

class ScriptTranslation : public Translation {
 public:
  ScriptTranslation(ScriptTranslator* translator,
                    Corrector* corrector,
                    Poet* poet,
                    const string& input,
                    size_t start,
                    size_t end_of_input,
                    const vector<Context::TabConstraint>& constraints,
                    int max_sentences,
                    double sentence_cutoff_threshold)
      : translator_(translator),
        poet_(poet),
        start_(start),
        end_of_input_(end_of_input),
        syllabifier_(
            New<ScriptSyllabifier>(translator, corrector, input, start)),
        constraints_(constraints),
        enable_correction_(corrector),
        max_sentences_(max_sentences),
        sentence_cutoff_threshold_(sentence_cutoff_threshold) {
    set_exhausted(true);
  }
  bool Evaluate(Dictionary* dict, UserDictionary* user_dict);
  virtual bool Next();
  virtual an<Candidate> Peek();

 protected:
  bool CheckEmpty();
  bool IsNormalSpelling() const;
  bool PrepareCandidate();
  template <class QueryResult>
  void EnrollEntries(map<int, DictEntryList>& entries_by_end_pos,
                     const an<QueryResult>& query_result);
  WordGraph PrepareForMakingSentence(Dictionary* dict,
                                     UserDictionary* user_dict);
  an<Sentence> MakeSentence(Dictionary* dict, UserDictionary* user_dict);
  deque<an<Sentence>> MakeSentences(Dictionary* dict,
                                    UserDictionary* user_dict);

  ScriptTranslator* translator_;
  Poet* poet_;
  size_t start_;
  size_t end_of_input_;
  an<ScriptSyllabifier> syllabifier_;
  vector<Context::TabConstraint> constraints_;

  an<DictEntryCollector> phrase_;
  an<UserDictEntryCollector> user_phrase_;
  deque<an<Sentence>> sentences_;

  an<Phrase> candidate_ = nullptr;
  size_t candidate_index_ = 0;
  enum CandidateSource {
    kUninitialized,
    kUserPhrase,
    kSysPhrase,
    kSentence,
  };
  CandidateSource candidate_source_ = kUninitialized;

  DictEntryCollector::reverse_iterator phrase_iter_;
  UserDictEntryCollector::reverse_iterator user_phrase_iter_;

  const size_t max_corrections_ = 4;
  size_t correction_count_ = 0;
  int max_sentences_ = 1;
  double sentence_cutoff_threshold_ = 0.1;

  bool enable_correction_;
};

// ScriptTranslator implementation

ScriptTranslator::ScriptTranslator(const Ticket& ticket)
    : Translator(ticket), Memory(ticket), TranslatorOptions(ticket) {
  if (!engine_)
    return;
  if (Config* config = engine_->schema()->config()) {
    config->GetInt(name_space_ + "/spelling_hints", &spelling_hints_);
    config->GetInt(name_space_ + "/max_word_length", &max_word_length_);
    config->GetInt(name_space_ + "/core_word_length", &core_word_length_);
    config->GetBool(name_space_ + "/always_show_comments",
                    &always_show_comments_);
    config->GetBool(name_space_ + "/enable_correction", &enable_correction_);
    if (!config->GetBool(name_space_ + "/enable_word_completion",
                         &enable_word_completion_)) {
      enable_word_completion_ = enable_completion_;
    }
    config->GetInt(name_space_ + "/max_homophones", &max_homophones_);
    poet_.reset(new Poet(language(), config));
    if (an<ConfigList> rules = config->GetList(name_space_ + "/canonicalize")) {
      the<Projection> canonicalizer(new Projection());
      if (canonicalizer->Load(rules)) {
        canonicalizer_ = std::move(canonicalizer);
      }
    }
    // Reuse the complete deployed algebra, including transformations before
    // derive/fuzz and removal of derived branches by subsequent rules.
    if (auto algebra = config->GetList("speller/algebra")) {
      auto projection = New<FuzzySpellingProjection>();
      if (projection->Load(algebra))
        fuzzy_projection_ = std::move(projection);
    }
  }
  if (enable_correction_) {
    if (auto* corrector = Corrector::Require("corrector")) {
      corrector_.reset(corrector->Create(ticket));
    }
  }
}

ConstraintFilteredTranslation::ConstraintFilteredTranslation(
    an<Translation> translation,
    Dictionary* dict,
    const vector<Context::TabConstraint>& constraints)
    : CacheTranslation(translation), dict_(dict), constraints_(constraints) {
  if (!exhausted() && Peek() && !MatchesConstraints(Peek())) {
    Next();
  }
}

bool ConstraintFilteredTranslation::Next() {
  if (exhausted())
    return false;
  do {
    CacheTranslation::Next();
  } while (!exhausted() && Peek() && !MatchesConstraints(Peek()));
  return !exhausted();
}

bool ConstraintFilteredTranslation::MatchesConstraints(
    const an<Candidate>& candidate) {
  if (constraints_.empty())
    return true;

  auto phrase = As<Phrase>(Candidate::GetGenuineCandidate(candidate));
  if (!phrase)
    return false;

  vector<string> decoded;
  if (!dict_ || !dict_->Decode(phrase->code(), &decoded) || decoded.empty())
    return true;

  size_t syllable_index = 0;
  for (const auto& constraint : constraints_) {
    if (syllable_index >= decoded.size())
      return false;
    const string syllable = StripTones(decoded[syllable_index++]);
    const string label = StripTones(constraint.label);
    if (syllable.empty() || label.empty())
      return false;
    if (constraint.span == 1) {
      if (std::tolower(static_cast<unsigned char>(syllable[0])) !=
          std::tolower(static_cast<unsigned char>(label[0]))) {
        return false;
      }
    } else if (syllable != label) {
      return false;
    }
  }
  return true;
}

an<Translation> ScriptTranslator::Query(const string& input,
                                        const Segment& segment) {
  if (!dict_ || !dict_->loaded())
    return nullptr;
  if (!segment.HasAnyTagIn(tags_))
    return nullptr;
  const string shadow_input =
      engine_->context()->shadow_input(segment.start, segment.end);
  DLOG(INFO) << "ScriptTranslator::Query input='" << shadow_input << "' ["
             << segment.start << ", " << segment.end << ")";

  FinishSession();

  bool enable_user_dict =
      user_dict_ && user_dict_->loaded() && !IsUserDictDisabledFor(input);

  size_t end_of_input = engine_->context()->input().length();
  // the translator should survive translations it creates
  auto result = New<ScriptTranslation>(
      this, corrector_.get(), poet_.get(), shadow_input, segment.start,
      end_of_input, engine_->context()->tab_constraints(), max_sentences_,
      sentence_cutoff_threshold_);
  if (!result || !result->Evaluate(
                     dict_.get(), enable_user_dict ? user_dict_.get() : NULL)) {
    return nullptr;
  }
  an<Translation> deduped = New<DistinctTranslation>(result);
  if (contextual_suggestions_) {
    return poet_->ContextualWeighted(deduped, input, segment.start, this);
  }
  return deduped;
}

int ScriptTranslator::core_word_length() const {
  if (max_word_length_ <= 0) {
    return core_word_length_;
  }
  return (std::min)(core_word_length_, max_word_length_);
}

static bool exceed_upperlimit(int length, int upper_limit) {
  return upper_limit > 0 && length > upper_limit;
}

bool ScriptTranslator::SaveCommitEntry(CommitEntry& commit_entry) {
  if (exceed_upperlimit(commit_entry.Length(), max_word_length())) {
    UpdateElements(commit_entry);
  } else {
    commit_entry.Save();
  }
  return true;
}

bool ScriptTranslator::ConcatenatePhrases(CommitEntry& commit_entry,
                                          const vector<an<Phrase>>& phrases) {
  const int kCoreWordLength = core_word_length();
  const int n = phrases.size();
  for (int i = 0; i < n; ++i) {
    int cur_len = 0;
    int j = i;
    for (; j < n; ++j) {
      commit_entry.AppendPhrase(phrases.at(j));
      cur_len += phrases.at(j)->code().size();
      if (kCoreWordLength <= 0 || cur_len > kCoreWordLength) {
        break;
      }
      SaveCommitEntry(commit_entry);
    }
    if (kCoreWordLength > 0) {
      if (j == i) {
        SaveCommitEntry(commit_entry);
      }
      commit_entry.Clear();
    }
  }
  SaveCommitEntry(commit_entry);
  commit_entry.Clear();

  return true;
}

bool ScriptTranslator::ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                              const Segment& seg) {
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetSelectedCandidate()));
  bool recognized = Language::intelligible(phrase, this);
  if (recognized) {
    queue_.push_back(phrase);
  }

  if (!recognized || seg.status >= Segment::kConfirmed) {
    ConcatenatePhrases(commit_entry, queue_);
    queue_.clear();
  }

  return true;
}

string ScriptTranslator::FormatPreedit(const string& preedit) {
  string result = preedit;
  preedit_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::Spell(const Code& code) {
  string result;
  vector<string> syllables;
  if (!dict_ || !dict_->Decode(code, &syllables) || syllables.empty())
    return result;
  result = boost::algorithm::join(syllables, string(1, delimiters_.at(0)));
  comment_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::GetPrecedingText(size_t start) const {
  return !contextual_suggestions_ ? string()
         : start > 0 ? engine_->context()->composition().GetTextBefore(start)
                     : engine_->context()->commit_history().latest_text();
}

bool ScriptTranslator::UpdateElements(const CommitEntry& commit_entry) {
  bool update_elements = false;
  // avoid updating single character entries within a phrase which is
  // composed with single characters only
  if (commit_entry.elements.size() > 1) {
    for (const DictEntry* e : commit_entry.elements) {
      if (e->code.size() > 1) {
        update_elements = true;
        break;
      }
    }
  }
  if (update_elements) {
    for (const DictEntry* e : commit_entry.elements) {
      user_dict_->UpdateEntry(*e, 0);
    }
  }
  return true;
}

bool ScriptTranslator::Memorize(const CommitEntry& commit_entry) {
  UpdateElements(commit_entry);
  user_dict_->UpdateEntry(commit_entry, 1);
  return true;
}

static string NormalizeTabLabel(const string& label) {
  string normalized = StripTones(label);
  for (char& ch : normalized) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return normalized;
}

static string NormalizeT9TabLabel(const string& label) {
  string normalized = NormalizeTabLabel(label);
  for (const auto* umlaut : {"ü", "Ü"}) {
    size_t pos = 0;
    while ((pos = normalized.find(umlaut, pos)) != string::npos) {
      normalized.replace(pos, 2, "v");
      ++pos;
    }
  }
  return normalized;
}

bool ScriptTranslator::UseLiteralTabLabels(const string& input,
                                           size_t position) const {
  // Load retains a projection only when derive/fuzz rules are present. Keep
  // the old labels and matching behavior for every other schema/input path.
  if (!fuzzy_projection_ || !engine_ || !engine_->schema())
    return false;
  while (position < input.size() &&
         delimiters().find(input[position]) != string::npos)
    ++position;
  if (position >= input.size() || input[position] < '0' ||
      input[position] > '9')
    return false;
  const auto mapping = ParseAlgebraReverseMapping(engine_->schema()->config());
  return mapping.count(input[position]) != 0;
}

void ScriptTranslator::CollectSyllableTabs(size_t start_pos,
                                           vector<InputTabEntry>* tabs) const {
  if (!dict_ || !dict_->loaded() || !engine_ || !engine_->context())
    return;

  const string& input = engine_->context()->input();
  if (start_pos >= input.length())
    return;

  // Build and prune the full graph against the current tab constraints, then
  // inspect the node at start_pos. This keeps tree navigation aligned with the
  // active selection path instead of exposing the raw unfiltered graph.
  ScriptSyllabifier syllabifier(const_cast<ScriptTranslator*>(this),
                                corrector_.get(), input, 0);
  syllabifier.BuildSyllableGraph(*dict_->prism());
  if (auto* ctx = engine_->context()) {
    const auto& constraints = ctx->tab_constraints();
    if (!constraints.empty()) {
      syllabifier.ApplyTabConstraints(dict_.get(), constraints);
    }
  }
  const auto& graph = syllabifier.syllable_graph();

  // Parse xlit algebra rules to build reverse mapping: target_char →
  // source_chars E.g.
  // xlit/ABCDEFGHIJKLMNOPQRSTUVWXYZ/22233344455566677778889999/
  //   → '6' maps to {m, n, o}
  map<char, set<char>> xlit_reverse;
  if (engine_ && engine_->schema() && engine_->schema()->config()) {
    xlit_reverse = ParseAlgebraReverseMapping(engine_->schema()->config());
  }

  // Check if the input character at start_pos has an xlit/derive mapping
  char first_input_char = input[start_pos];
  bool first_char_has_xlit = xlit_reverse.count(first_input_char) > 0;

  const size_t first_syllable_tab = tabs->size();
  map<string, size_t> seen;
  bool has_single_span = false;
  // On T9, show only syllables whose literal spelling matches this edge.
  // Fuzzy syllables remain in the candidate graph. Keep non-digit input tabs
  // and the independent initial/raw-key tabs on their existing paths.
  auto* literal =
      UseLiteralTabLabels(input, start_pos) ? fuzzy_projection() : nullptr;
  auto it = graph.edges.find(start_pos);
  if (it != graph.edges.end()) {
    for (const auto& [end_pos, syllable_map] : it->second) {
      if (end_pos == start_pos + 1) {
        has_single_span = true;
        if (first_char_has_xlit)
          continue;  // Skip span=1 noise when mapping applies
      }
      set<int> typed_spellings;
      if (literal) {
        typed_spellings = syllabifier.MatchLiteralSpellings(start_pos, end_pos,
                                                            *dict_->prism());
      }
      for (const auto& [syllable_id, props] : syllable_map) {
        Code single_code;
        single_code.push_back(syllable_id);
        vector<string> decoded;
        if (dict_->Decode(single_code, &decoded) && !decoded.empty()) {
          if (literal && !literal->SpellsLiterally(decoded[0], typed_spellings,
                                                   *dict_->prism()))
            continue;
          string label = literal ? NormalizeT9TabLabel(decoded[0])
                                 : NormalizeTabLabel(decoded[0]);
          if (!label.empty()) {
            size_t span = end_pos - start_pos;
            auto [it, inserted] = seen.emplace(label, tabs->size());
            if (inserted) {
              tabs->emplace_back(
                  InputTabEntry{label, span, InputTabEntry::kSyllable});
            } else if (span > (*tabs)[it->second].span) {
              (*tabs)[it->second].span = span;
            }
          }
        }
      }
    }
  }

  // Longest syllables first (e.g. "sheng" before "shen" before "she"), and
  // alphabetical within the same span. The internal syllable-id order is not
  // alphabetical (e.g. "fo" can precede "en"), which looks random in the bar.
  // Only the syllable tabs collected above are reordered; tabs from other
  // translators and the xlit/raw-key tabs appended below keep their positions.
  std::stable_sort(tabs->begin() + first_syllable_tab, tabs->end(),
                   [](const InputTabEntry& a, const InputTabEntry& b) {
                     if (a.span != b.span)
                       return a.span > b.span;
                     return a.label < b.label;
                   });

  // Generate xlit partial-match tabs for 简拼 disambiguation
  if ((!has_single_span || first_char_has_xlit) && first_char_has_xlit) {
    auto xlit_it = xlit_reverse.find(first_input_char);
    if (xlit_it != xlit_reverse.end()) {
      for (char letter : xlit_it->second) {
        string label(1, static_cast<char>(
                            std::tolower(static_cast<unsigned char>(letter))));
        tabs->emplace_back(InputTabEntry{label, 1, InputTabEntry::kSyllable});
      }
      string first_char(1, first_input_char);
      tabs->emplace_back(
          InputTabEntry{first_char, 1, InputTabEntry::kRawInput});
    }
  }
}

// ScriptSyllabifier implementation

Spans ScriptSyllabifier::Syllabify(const Phrase* phrase) {
  Spans result;
  vector<size_t> vertices;
  vertices.push_back(start_);
  SyllabifyTask task{
      phrase->code(), syllable_graph_, phrase->end() - start_,
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) { vertices.push_back(start_ + next_pos); },
      [&](SyllabifyTask* task, size_t depth) { vertices.pop_back(); }};
  if (syllabify_dfs(&task, 0, phrase->start() - start_)) {
    result.set_vertices(std::move(vertices));
  }
  return result;
}

size_t ScriptSyllabifier::BuildSyllableGraph(Prism& prism) {
  return (size_t)syllabifier_.BuildSyllableGraph(input_, prism,
                                                 &syllable_graph_);
}

// Helper: decode a single SyllableId to string
static string DecodeSyllableId(Dictionary* dict, SyllableId id) {
  if (!dict)
    return "";
  Code code;
  code.push_back(id);
  vector<string> decoded;
  if (dict->Decode(code, &decoded) && !decoded.empty())
    return decoded[0];
  return "";
}

// Returns true when the schema's spelling algebra derives `label` from
// `syllable`, that is, when the prism also indexes `syllable` under the
// spelling `label`. Only derive/fuzz rules establish fuzzy provenance; the
// other algebra rules propagate or remove branches in deployment order.
// A schema without fuzzy rules keeps the exact-match behavior.
static bool FuzzySpellingMatches(FuzzySpellingProjection* projection,
                                 const string& syllable,
                                 const string& label,
                                 const set<int>& matched_spellings,
                                 Prism& prism,
                                 bool normalize_t9) {
  if (!projection || syllable.empty() || label.empty())
    return false;
  return projection->Matches(syllable, label, matched_spellings, prism,
                             normalize_t9);
}

void ScriptSyllabifier::ApplyTabConstraints(
    Dictionary* dict,
    const vector<Context::TabConstraint>& constraints) {
  if (constraints.empty())
    return;
  DLOG(INFO) << "ApplyTabConstraints: " << constraints.size()
             << " constraints, start_=" << start_
             << " input_length=" << syllable_graph_.input_length;

  for (const auto& constraint : constraints) {
    if (constraint.position < start_)
      continue;
    size_t graph_pos = constraint.position - start_;
    DLOG(INFO) << "  constraint: pos=" << constraint.position
               << " graph_pos=" << graph_pos << " label=[" << constraint.label
               << "]" << " span=" << constraint.span;
    if (graph_pos >= syllable_graph_.input_length) {
      DLOG(INFO) << "  SKIP: graph_pos >= input_length";
      continue;
    }

    auto edge_it = syllable_graph_.edges.find(graph_pos);
    if (edge_it == syllable_graph_.edges.end()) {
      DLOG(INFO) << "  SKIP: no edges at graph_pos=" << graph_pos;
      continue;
    }

    const bool normalize_t9 =
        translator_ && translator_->UseLiteralTabLabels(input_, graph_pos);
    string label_base = normalize_t9 ? NormalizeT9TabLabel(constraint.label)
                                     : StripTones(constraint.label);
    bool is_single_char = (constraint.span == 1);

    // A full-syllable tab may stand for a syllable whose canonical spelling
    // differs from the label because the schema's fuzzy-spelling algebra also
    // spells it as the label (for example, tab "shen" is how "sheng" is typed
    // under en/eng fuzzy spelling).  Such a syllable is already indexed under
    // the selected spelling, so keep it instead of dropping it as a mismatch.
    auto* fuzzy = translator_ ? translator_->fuzzy_projection() : nullptr;

    auto& end_map = edge_it->second;
    DLOG(INFO) << "  label_base=[" << label_base
               << "] is_single=" << is_single_char
               << " end_map_size=" << end_map.size();

    size_t total_before = 0;
    for (const auto& [ep, sm] : end_map) {
      total_before += sm.size();
      DLOG(INFO) << "    end_pos=" << ep << " syllables=" << sm.size();
    }
    DLOG(INFO) << "  total_syllables_before=" << total_before;
    for (auto end_it = end_map.begin(); end_it != end_map.end();) {
      size_t end_pos = end_it->first;
      auto& spelling_map = end_it->second;  // map<SyllableId, EdgeProperties>

      // A single-key initial may represent a syllable spanning multiple input
      // positions (for example, T9 "n" matches "ni"). Full syllables must
      // still consume exactly the selected input span.
      if (!is_single_char && end_pos != graph_pos + constraint.span) {
        end_it = end_map.erase(end_it);
        continue;
      }

      set<int> exact_spellings, completion_spellings;
      if (!is_single_char && fuzzy) {
        exact_spellings = syllabifier_.MatchSpellings(
            input_, graph_pos, end_pos, *dict->prism(), false);
        if (std::any_of(spelling_map.begin(), spelling_map.end(),
                        [](const auto& entry) {
                          return entry.second.type == kCompletion;
                        })) {
          completion_spellings = syllabifier_.MatchSpellings(
              input_, graph_pos, end_pos, *dict->prism(), true);
        }
      }

      // Filter syllables in this edge
      for (auto syl_it = spelling_map.begin(); syl_it != spelling_map.end();) {
        string decoded = DecodeSyllableId(dict, syl_it->first);
        string decoded_base =
            normalize_t9 ? NormalizeT9TabLabel(decoded) : StripTones(decoded);
        bool keep = false;

        if (is_single_char) {
          // 简拼: match first letter
          if (!decoded_base.empty() &&
              std::tolower(decoded_base[0]) == std::tolower(label_base[0])) {
            keep = true;
          }
        } else {
          // Full syllable: tone-insensitive match, plus syllables that the
          // schema's fuzzy-spelling rules also spell as the selected label.
          if (decoded_base == label_base ||
              FuzzySpellingMatches(fuzzy, decoded, label_base,
                                   syl_it->second.type == kCompletion
                                       ? completion_spellings
                                       : exact_spellings,
                                   *dict->prism(), normalize_t9)) {
            keep = true;
          }
        }

        DLOG(INFO) << "      syl_id=" << syl_it->first << " decoded=["
                   << decoded << "]" << " base=[" << decoded_base << "]"
                   << " keep=" << keep;

        if (keep)
          ++syl_it;
        else
          syl_it = spelling_map.erase(syl_it);
      }

      // If no syllables left in this edge, remove the edge
      if (spelling_map.empty())
        end_it = end_map.erase(end_it);
      else
        ++end_it;
    }
    size_t total_after = 0;
    for (const auto& [ep, sm] : end_map)
      total_after += sm.size();
    DLOG(INFO) << "  total_syllables_after=" << total_after
               << " end_map_size_after=" << end_map.size();
  }

  // Rebuild indices from filtered edges (Transpose)
  syllable_graph_.indices.clear();
  for (const auto& start : syllable_graph_.edges) {
    auto& index(syllable_graph_.indices[start.first]);
    for (const auto& end : start.second) {
      for (const auto& spelling : end.second) {
        SyllableId syll_id = spelling.first;
        index[syll_id].push_back(&spelling.second);
      }
    }
  }
}

bool ScriptSyllabifier::IsCorrection(const Code& code,
                                     size_t code_length) const {
  vector<bool> path_attributes;
  path_attributes.reserve(8);

  SyllabifyTask task{
      code, syllable_graph_, code_length,
      // push
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) {
        auto id = task->code[depth];

        auto start_iter = syllable_graph_.edges.find(current_pos);
        if (start_iter != syllable_graph_.edges.end()) {
          auto end_iter = start_iter->second.find(next_pos);
          if (end_iter != start_iter->second.end()) {
            auto prop_iter = end_iter->second.find(id);
            if (prop_iter != end_iter->second.end()) {
              path_attributes.push_back(prop_iter->second.is_correction);
              return;
            }
          }
        }
        // edge not found
        path_attributes.push_back(false);
      },
      // pop
      [&](SyllabifyTask* task, size_t depth) { path_attributes.pop_back(); }};

  if (syllabify_dfs(&task, 0, 0)) {
    for (bool is_correction : path_attributes) {
      if (is_correction)
        return true;
    }
  }
  return false;
}

string ScriptSyllabifier::GetPreeditString(const Phrase& cand) const {
  const auto& delimiters = translator_->delimiters();
  std::stack<size_t> lengths;
  string output;
  SyllabifyTask task{cand.matching_code(), syllable_graph_, cand.end() - start_,
                     [&](SyllabifyTask* task, size_t depth, size_t current_pos,
                         size_t next_pos) {
                       size_t len = output.length();
                       if (depth > 0 && len > 0 &&
                           delimiters.find(output[len - 1]) == string::npos) {
                         output += delimiters.at(0);
                       }
                       output +=
                           input_.substr(current_pos, next_pos - current_pos);
                       lengths.push(len);
                     },
                     [&](SyllabifyTask* task, size_t depth) {
                       output.resize(lengths.top());
                       lengths.pop();
                     }};
  if (syllabify_dfs(&task, 0, cand.start() - start_)) {
    return translator_->FormatPreedit(output);
  } else {
    return string();
  }
}

string ScriptSyllabifier::GetOriginalSpelling(const Phrase& cand) const {
  if (translator_ &&
      static_cast<int>(cand.code().size()) <= translator_->spelling_hints()) {
    return translator_->Spell(cand.code());
  }
  return string();
}

template <class Ptr, class Iter>
static bool has_exact_match_phrase(Ptr ptr, Iter iter, size_t consumed) {
  return ptr && iter->first == consumed && !iter->second.exhausted() &&
         iter->second.Peek()->IsExactMatch();
}

// ScriptTranslation implementation

bool ScriptTranslation::Evaluate(Dictionary* dict, UserDictionary* user_dict) {
  size_t consumed = syllabifier_->BuildSyllableGraph(*dict->prism());
  if (!constraints_.empty()) {
    syllabifier_->ApplyTabConstraints(dict, constraints_);
  }

  const auto& syllable_graph = syllabifier_->syllable_graph();
  bool predict_word = translator_->enable_word_completion() &&
                      start_ + consumed == end_of_input_;

  phrase_ =
      dict->Lookup(syllable_graph, 0, &translator_->blacklist(), predict_word);
  if (user_dict) {
    const size_t kUnlimitedDepth = 0;
    const size_t kNumSyllablesToPredictWord = 4;
    user_phrase_ =
        user_dict->Lookup(syllable_graph, 0, kUnlimitedDepth,
                          predict_word ? kNumSyllablesToPredictWord : 0);
  }
  if (!phrase_ && !user_phrase_)
    return false;

  if (phrase_)
    phrase_iter_ = phrase_->rbegin();
  if (user_phrase_)
    user_phrase_iter_ = user_phrase_->rbegin();

  auto is_correction_match = [&](auto& iter_pair, size_t len) {
    if (iter_pair.first != len || iter_pair.second.exhausted())
      return false;
    return syllabifier_->IsCorrection(iter_pair.second.Peek()->code, len);
  };

  bool has_reliable_phrase =
      has_exact_match_phrase(phrase_, phrase_iter_, consumed) &&
      !is_correction_match(*phrase_iter_, consumed);

  bool has_reliable_user_phrase =
      has_exact_match_phrase(user_phrase_, user_phrase_iter_, consumed) &&
      !is_correction_match(*user_phrase_iter_, consumed);

  bool has_at_least_two_syllables = syllable_graph.edges.size() >= 2;
  DLOG(INFO) << "consumed: " << consumed
             << ", has_reliable_phrase: " << has_reliable_phrase
             << ", has_reliable_user_phrase: " << has_reliable_user_phrase
             << ", has_at_least_two_syllables: " << has_at_least_two_syllables;
  // make sentences when there is no exact-matching phrase candidate
  if (has_at_least_two_syllables && !has_reliable_phrase &&
      !has_reliable_user_phrase) {
    if (max_sentences_ > 1)
      sentences_ = MakeSentences(dict, user_dict);
    else if (max_sentences_) {
      auto sentence = MakeSentence(dict, user_dict);
      if (sentence)
        sentences_ = {sentence};
      else
        sentences_.clear();
    }
  }

  return !CheckEmpty();
}

bool ScriptTranslation::Next() {
  do {
    if (exhausted())
      return false;
    if (candidate_source_ == kUninitialized) {
      PrepareCandidate();  // to determine candidate_source_
    }
    switch (candidate_source_) {
      case kUninitialized:
        break;
      case kSentence:
        if (!sentences_.empty())
          sentences_.pop_front();
        break;
      case kUserPhrase: {
        UserDictEntryIterator& uter(user_phrase_iter_->second);
        if (!uter.Next()) {
          ++user_phrase_iter_;
        }
      } break;
      case kSysPhrase: {
        DictEntryIterator& iter(phrase_iter_->second);
        if (!iter.Next()) {
          ++phrase_iter_;
        }
      } break;
    }
    candidate_.reset();
    candidate_source_ = kUninitialized;
    if (enable_correction_) {
      // populate next candidate and skip it if it's a correction beyond max
      // numbers.
      if (!PrepareCandidate()) {
        break;
      }
    }
  } while (enable_correction_ &&
           syllabifier_->IsCorrection(candidate_->code(),
                                      candidate_->end() - start_) &&
           // limit the number of correction candidates
           ++correction_count_ > max_corrections_);
  if (!CheckEmpty()) {
    ++candidate_index_;
    return true;
  }
  return false;
}

an<Candidate> ScriptTranslation::Peek() {
  if (candidate_source_ == kUninitialized && !PrepareCandidate()) {
    return nullptr;
  }
  if (candidate_->preedit().empty()) {
    candidate_->set_preedit(syllabifier_->GetPreeditString(*candidate_));
  }
  if (candidate_->comment().empty()) {
    auto spelling = syllabifier_->GetOriginalSpelling(*candidate_);
    if (!spelling.empty() && (translator_->always_show_comments() ||
                              spelling != candidate_->preedit())) {
      candidate_->set_comment(/*quote_left + */ spelling /* + quote_right*/);
    }
  }
  candidate_->set_syllabifier(syllabifier_);
  return candidate_;
}

static bool always_true() {
  return true;
}

template <typename T>
inline static bool prefer_user_phrase(
    T user_phrase_weight,
    T sys_phrase_weight,
    function<bool()> compare_on_tie = always_true) {
  return user_phrase_weight > sys_phrase_weight ||
         (user_phrase_weight == sys_phrase_weight && compare_on_tie());
}

bool ScriptTranslation::PrepareCandidate() {
iter_incremented:
  if (exhausted()) {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
  if (!sentences_.empty()) {
    candidate_source_ = kSentence;
    candidate_ = sentences_[0];
    return true;
  }
  const size_t full_code_length = end_of_input_ - start_;
  size_t user_phrase_code_length = 0;
  if (user_phrase_ && user_phrase_iter_ != user_phrase_->rend()) {
    user_phrase_code_length = user_phrase_iter_->first;
  }
  size_t phrase_code_length = 0;
  if (phrase_ && phrase_iter_ != phrase_->rend()) {
    phrase_code_length = phrase_iter_->first;
  }
  if (user_phrase_code_length > 0 &&
      prefer_user_phrase(
          user_phrase_code_length, phrase_code_length,
          // 編碼長度相同時, 用戶詞優先
          [this, full_code_length, phrase_code_length]() {
            UserDictEntryIterator& uter = user_phrase_iter_->second;
            DictEntryIterator& iter = phrase_iter_->second;
            // 但若用戶詞爲糾錯結果, 則免去優先權, 回歸權重比較
            bool user_is_correction = syllabifier_->IsCorrection(
                uter.Peek()->code, phrase_code_length);
            if (user_is_correction) {
              // 因對糾錯候選施加了較大懲罰, 通常結果爲 系統原文 > 用戶糾錯
              // 兩者皆糾錯時, 用戶詞的動態權重有助達成 用戶糾錯 > 系統糾錯
              return uter.Peek()->weight >= iter.Peek()->weight;
            }
            // 長詞聯想之前須至少出一個嚴格匹配的候選
            // 故確定首選之際, 系統嚴格匹配 > 用戶長詞聯想
            const int kNumExactMatchOnTop = 1;
            return candidate_index_ >= kNumExactMatchOnTop ||
                   prefer_user_phrase(
                       has_exact_match_phrase(user_phrase_, user_phrase_iter_,
                                              full_code_length),
                       has_exact_match_phrase(phrase_, phrase_iter_,
                                              full_code_length));
          })) {
    UserDictEntryIterator& uter = user_phrase_iter_->second;
    const auto& entry = uter.Peek();
    DLOG(INFO) << "user phrase '" << entry->text
               << "', code length: " << user_phrase_code_length;
    candidate_source_ = kUserPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "user_phrase",
                    start_, start_ + user_phrase_code_length, entry);
    candidate_->set_dictionary(translator_->primary_dictionary());
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else if (phrase_code_length > 0) {
    DictEntryIterator& iter = phrase_iter_->second;
    if (iter.exhausted()) {
      ++phrase_iter_;
      goto iter_incremented;
    }
    const auto& entry = iter.Peek();
    DLOG(INFO) << "phrase '" << entry->text
               << "', code length: " << phrase_code_length;
    candidate_source_ = kSysPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "phrase",
                    start_, start_ + phrase_code_length, entry);
    candidate_->set_dictionary(translator_->primary_dictionary());
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
}

bool ScriptTranslation::CheckEmpty() {
  set_exhausted((!phrase_ || phrase_iter_ == phrase_->rend()) &&
                (!user_phrase_ || user_phrase_iter_ == user_phrase_->rend()));
  return exhausted();
}

template <class QueryResult>
void ScriptTranslation::EnrollEntries(
    map<int, DictEntryList>& entries_by_end_pos,
    const an<QueryResult>& query_result) {
  if (query_result) {
    for (auto& y : *query_result) {
      DictEntryList& homophones = entries_by_end_pos[y.first];
      while (homophones.size() < translator_->max_homophones() &&
             !y.second.exhausted()) {
        homophones.push_back(y.second.Peek());
        if (!y.second.Next())
          break;
      }
    }
  }
}

WordGraph ScriptTranslation::PrepareForMakingSentence(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const int kMaxSyllablesForUserPhraseQuery = 5;
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph;
  for (const auto& x : syllable_graph.edges) {
    auto& same_start_pos = graph[x.first];
    if (user_dict) {
      EnrollEntries(same_start_pos,
                    user_dict->Lookup(syllable_graph, x.first,
                                      kMaxSyllablesForUserPhraseQuery));
    }
    // merge lookup results
    EnrollEntries(same_start_pos, dict->Lookup(syllable_graph, x.first,
                                               &translator_->blacklist()));
  }
  return graph;
}

deque<an<Sentence>> ScriptTranslation::MakeSentences(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  auto sentences =
      poet_->MakeSentences(graph, syllable_graph.interpreted_length,
                           translator_->GetPrecedingText(start_),
                           max_sentences_, sentence_cutoff_threshold_);
  for (auto& sentence : sentences) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
    sentence->set_dictionary(dict);
  }
  return sentences;
}

an<Sentence> ScriptTranslation::MakeSentence(Dictionary* dict,
                                             UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  if (auto sentence =
          poet_->MakeSentence(graph, syllable_graph.interpreted_length,
                              translator_->GetPrecedingText(start_))) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
    sentence->set_dictionary(dict);
    return sentence;
  }
  return nullptr;
}

}  // namespace rime
