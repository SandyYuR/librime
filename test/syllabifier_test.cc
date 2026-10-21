//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-05 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <rime/dict/prism.h>
#include <rime/algo/algebra.h>
#include <rime/algo/syllabifier.h>

using namespace rime;

class RimeSyllabifierTest : public ::testing::Test {
 public:
  virtual void SetUp() {
    vector<string> syllables;
    syllables.push_back("a");      // 0 == id
    syllables.push_back("an");     // 1
    syllables.push_back("cha");    // 2
    syllables.push_back("chan");   // 3
    syllables.push_back("chang");  // 4
    syllables.push_back("gan");    // 5
    syllables.push_back("han");    // 6
    syllables.push_back("hang");   // 7
    syllables.push_back("na");     // 8
    syllables.push_back("tu");     // 9
    syllables.push_back("tuan");   // 10
    std::sort(syllables.begin(), syllables.end());
    for (size_t i = 0; i < syllables.size(); ++i) {
      syllable_id_[syllables[i]] = i;
    }

    path file_path("syllabifier_test.bin");
    prism_.reset(new Prism(file_path));
    set<string> keyset;
    std::copy(syllables.begin(), syllables.end(),
              std::inserter(keyset, keyset.begin()));
    prism_->Build(keyset);
  }

  virtual void TearDown() {
    std::error_code ec;
    std::filesystem::remove("syllabifier_test.bin", ec);
  }

 protected:
  map<string, SyllableId> syllable_id_;
  the<Prism> prism_;
};

TEST_F(RimeSyllabifierTest, CaseAlpha) {
  Syllabifier s;
  SyllableGraph g;
  const string input("a");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(2, g.vertices.size());
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(1));
  EXPECT_EQ(kNormalSpelling, g.vertices[1]);
  SpellingMap& sp(g.edges[0][1]);
  EXPECT_EQ(1, sp.size());
  ASSERT_FALSE(sp.end() == sp.find(syllable_id_["a"]));
  EXPECT_EQ(kNormalSpelling, sp[0].type);
  EXPECT_EQ(0.0, sp[0].credibility);
}

TEST_F(RimeSyllabifierTest, CaseFailure) {
  Syllabifier s;
  SyllableGraph g;
  const string input("ang");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length() - 1, g.interpreted_length);
  EXPECT_EQ(2, g.vertices.size());
  ASSERT_TRUE(g.vertices.end() == g.vertices.find(1));
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(2));
  EXPECT_EQ(kNormalSpelling, g.vertices[2]);
  SpellingMap& sp(g.edges[0][2]);
  EXPECT_EQ(1, sp.size());
  ASSERT_FALSE(sp.end() == sp.find(syllable_id_["an"]));
}

TEST_F(RimeSyllabifierTest, CaseChangan) {
  Syllabifier s;
  SyllableGraph g;
  const string input("changan");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(4, g.vertices.size());
  // not c'han'gan or c'hang'an
  EXPECT_TRUE(g.vertices.end() == g.vertices.find(1));
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(4));
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(5));
  EXPECT_EQ(kNormalSpelling, g.vertices[4]);
  EXPECT_EQ(kNormalSpelling, g.vertices[5]);
  // chan, chang but not cha
  EndVertexMap& e0(g.edges[0]);
  EXPECT_EQ(2, e0.size());
  ASSERT_FALSE(e0.end() == e0.find(4));
  ASSERT_FALSE(e0.end() == e0.find(5));
  EXPECT_FALSE(e0[4].end() == e0[4].find(syllable_id_["chan"]));
  EXPECT_FALSE(e0[5].end() == e0[5].find(syllable_id_["chang"]));
  // gan$
  EndVertexMap& e4(g.edges[4]);
  EXPECT_EQ(1, e4.size());
  ASSERT_FALSE(e4.end() == e4.find(7));
  EXPECT_FALSE(e4[7].end() == e4[7].find(syllable_id_["gan"]));
  // an$
  EndVertexMap& e5(g.edges[5]);
  EXPECT_EQ(1, e5.size());
  ASSERT_FALSE(e5.end() == e5.find(7));
  EXPECT_FALSE(e5[7].end() == e5[7].find(syllable_id_["an"]));
}

TEST_F(RimeSyllabifierTest, CaseTuan) {
  Syllabifier s;
  SyllableGraph g;
  const string input("tuan");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(3, g.vertices.size());
  // both tu'an and tuan
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(2));
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(4));
  EXPECT_EQ(kAmbiguousSpelling, g.vertices[2]);
  EXPECT_EQ(kNormalSpelling, g.vertices[4]);
  EndVertexMap& e0(g.edges[0]);
  EXPECT_EQ(2, e0.size());
  ASSERT_FALSE(e0.end() == e0.find(2));
  ASSERT_FALSE(e0.end() == e0.find(4));
  EXPECT_FALSE(e0[2].end() == e0[2].find(syllable_id_["tu"]));
  EXPECT_FALSE(e0[4].end() == e0[4].find(syllable_id_["tuan"]));
  // an$
  EndVertexMap& e2(g.edges[2]);
  EXPECT_EQ(1, e2.size());
  ASSERT_FALSE(e2.end() == e2.find(4));
  EXPECT_FALSE(e2[4].end() == e2[4].find(syllable_id_["an"]));
}

TEST_F(RimeSyllabifierTest, CaseChainingAmbiguity) {
  Syllabifier s;
  SyllableGraph g;
  const string input("anana");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(input.length() + 1, g.vertices.size());
}

TEST_F(RimeSyllabifierTest, TransposedSyllableGraph) {
  Syllabifier s;
  SyllableGraph g;
  const string input("changan");
  s.BuildSyllableGraph(input, *prism_, &g);
  ASSERT_FALSE(g.indices.end() == g.indices.find(0));
  EXPECT_EQ(2, g.indices[0].size());
  EXPECT_FALSE(g.indices[0].end() == g.indices[0].find(syllable_id_["chan"]));
  EXPECT_FALSE(g.indices[0].end() == g.indices[0].find(syllable_id_["chang"]));
  ASSERT_EQ(1, g.indices[0][syllable_id_["chan"]].size());
  ASSERT_FALSE(NULL == g.indices[0][syllable_id_["chan"]][0]);
  EXPECT_EQ(4, g.indices[0][syllable_id_["chan"]][0]->end_pos);
}

TEST_F(RimeSyllabifierTest, TrimLeadingDelimiters) {
  Syllabifier s(" '");
  SyllableGraph g;
  const string input("''a");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(2, g.vertices.size());
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(3));
  EXPECT_EQ(kNormalSpelling, g.vertices[1]);
  SpellingMap& sp(g.edges[0][3]);
  EXPECT_EQ(1, sp.size());
  ASSERT_FALSE(sp.end() == sp.find(syllable_id_["a"]));
  EXPECT_EQ(kNormalSpelling, sp[0].type);
  EXPECT_EQ(0.0, sp[0].credibility);
}

TEST_F(RimeSyllabifierTest, TrimTrailingDelimiters) {
  Syllabifier s(" '");
  SyllableGraph g;
  const string input("a''");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(2, g.vertices.size());
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(3));
  EXPECT_EQ(kNormalSpelling, g.vertices[1]);
  SpellingMap& sp(g.edges[0][3]);
  EXPECT_EQ(1, sp.size());
  ASSERT_FALSE(sp.end() == sp.find(syllable_id_["a"]));
  EXPECT_EQ(kNormalSpelling, sp[0].type);
  EXPECT_EQ(0.0, sp[0].credibility);
}

TEST_F(RimeSyllabifierTest, TrimBothLeadingAndTrailingDelimiters) {
  Syllabifier s(" '");
  SyllableGraph g;
  const string input("''a''");
  s.BuildSyllableGraph(input, *prism_, &g);
  EXPECT_EQ(input.length(), g.input_length);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(2, g.vertices.size());
  ASSERT_FALSE(g.vertices.end() == g.vertices.find(5));
  EXPECT_EQ(kNormalSpelling, g.vertices[1]);
  SpellingMap& sp(g.edges[0][5]);
  EXPECT_EQ(1, sp.size());
  ASSERT_FALSE(sp.end() == sp.find(syllable_id_["a"]));
  EXPECT_EQ(kNormalSpelling, sp[0].type);
  EXPECT_EQ(0.0, sp[0].credibility);
}

class CanonicalizeSyllabifierTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_prism_path_ =
        std::filesystem::temp_directory_path() / "test_canonicalize.prism.bin";

    // 宮保拼音並擊碼
    Syllabary syllabary = {
        "ZF",     // zhi
        "ZFUR",   // zhui
        "ZFURO",  // zhong
        "SA",     // sa
        "HE",     // he
        "GE",     // ge
        "HGE"     // re
    };

    Prism prism(test_prism_path_);
    ASSERT_TRUE(prism.Build(syllabary));
    ASSERT_TRUE(prism.Save());

    loaded_prism_ = std::make_unique<Prism>(test_prism_path_);
    ASSERT_TRUE(loaded_prism_->Load());

    auto rules = New<ConfigList>();
    // 宮保拼音 3.0 標準鍵序
    rules->Append(New<ConfigValue>("reorder SCZHLFGDBKTPIUÜANREO"));
    canonicalizer_ = std::make_unique<Projection>();
    ASSERT_TRUE(canonicalizer_->Load(rules));
  }

  void TearDown() override {
    loaded_prism_.reset();
    if (std::filesystem::exists(test_prism_path_)) {
      std::filesystem::remove(test_prism_path_);
    }
  }

  std::filesystem::path test_prism_path_;
  std::unique_ptr<Prism> loaded_prism_;
  std::unique_ptr<Projection> canonicalizer_;
};

// 測試 1: 基礎聲母同手倒錯 (FZ -> ZF) 與多鍵倒錯 (FZURO -> ZFURO)
TEST_F(CanonicalizeSyllabifierTest, BasicInversion) {
  Syllabifier syllabifier("'", false, false, canonicalizer_.get());
  SyllableGraph graph;

  // 輸入亂序碼 FZURO (意圖打 zhong)，標準碼為 ZFURO
  int farthest =
      syllabifier.BuildSyllableGraph("FZURO", *loaded_prism_, &graph);

  EXPECT_EQ(5, farthest);
  EXPECT_EQ(5, graph.interpreted_length);

  // 驗證圖上是否存在從起點 0 到終點 5 的邊
  ASSERT_NE(graph.edges.find(0), graph.edges.end());
  auto& end_vertices = graph.edges[0];
  ASSERT_NE(end_vertices.find(5), end_vertices.end());

  // 取得該邊匹配到的音節 ID，確認對應 Prism 內的 "ZFURO"
  int expected_syll_id = -1;
  ASSERT_TRUE(loaded_prism_->GetValue("ZFURO", &expected_syll_id));
  EXPECT_NE(end_vertices[5].find(expected_syll_id), end_vertices[5].end());
}

// 測試 2: 雙手交錯落鍵 (UZRF -> ZFUR)
TEST_F(CanonicalizeSyllabifierTest, CrossHandInterleaving) {
  Syllabifier syllabifier("'", false, false, canonicalizer_.get());
  SyllableGraph graph;

  // 右手 U/R 與左手 Z/F 交錯落鍵: U-Z-R-F
  int farthest = syllabifier.BuildSyllableGraph("UZRF", *loaded_prism_, &graph);

  EXPECT_EQ(4, farthest);
  int expected_syll_id = -1;
  ASSERT_TRUE(loaded_prism_->GetValue("ZFUR", &expected_syll_id));

  ASSERT_NE(graph.edges[0].find(4), graph.edges[0].end());
  EXPECT_NE(graph.edges[0][4].find(expected_syll_id), graph.edges[0][4].end());
}

// 測試 3: 連續多音節串流切分 (FZURO'SA 與無分隔符 FZUROSA)
TEST_F(CanonicalizeSyllabifierTest, MultiSyllableStreaming) {
  Syllabifier syllabifier("'", false, false, canonicalizer_.get());

  int id_zfuro = -1;
  int id_sa = -1;
  ASSERT_TRUE(loaded_prism_->GetValue("ZFURO", &id_zfuro));
  ASSERT_TRUE(loaded_prism_->GetValue("SA", &id_sa));

  // 情況 A: 帶隔音符號 FZURO'SA
  {
    SyllableGraph graph;
    int farthest =
        syllabifier.BuildSyllableGraph("FZURO'SA", *loaded_prism_, &graph);
    EXPECT_EQ(8, farthest);

    // 第一音節: 匹配長度 5 + 吞入 1 位分隔符 '\'' -> 終點爲 6，即邊 [0, 6)
    ASSERT_NE(graph.edges[0].find(6), graph.edges[0].end());
    EXPECT_NE(graph.edges[0][6].find(id_zfuro), graph.edges[0][6].end());

    // 第二音節: 承接頂點 6，終點爲 8，即邊 [6, 8)
    ASSERT_NE(graph.edges.find(6), graph.edges.end());
    ASSERT_NE(graph.edges[6].find(8), graph.edges[6].end());
    EXPECT_NE(graph.edges[6][8].find(id_sa), graph.edges[6][8].end());
  }

  // 情況 B: 無隔音符號連續連打 FZUROSA
  {
    SyllableGraph graph;
    int farthest =
        syllabifier.BuildSyllableGraph("FZUROSA", *loaded_prism_, &graph);
    EXPECT_EQ(7, farthest);

    // 第一音節邊: [0, 5)
    ASSERT_NE(graph.edges[0].find(5), graph.edges[0].end());
    EXPECT_NE(graph.edges[0][5].find(id_zfuro), graph.edges[0][5].end());

    // 第二音節邊: [5, 7)
    ASSERT_NE(graph.edges.find(5), graph.edges.end());
    ASSERT_NE(graph.edges[5].find(7), graph.edges[5].end());
    EXPECT_NE(graph.edges[5][7].find(id_sa), graph.edges[5][7].end());
  }
}

// 測試 4: 回退保證 (當 canonicalizer 爲 nullptr 時恢復原生前綴匹配)
TEST_F(CanonicalizeSyllabifierTest, NullCanonicalizerFallback) {
  // 不傳入 canonicalizer_，此時應走原生 CommonPrefixSearch
  Syllabifier syllabifier("'", false, false, nullptr);
  SyllableGraph graph;

  // 1. 輸入標準順序 ZFURO -> 必須成功
  int farthest_ordered =
      syllabifier.BuildSyllableGraph("ZFURO", *loaded_prism_, &graph);
  EXPECT_EQ(5, farthest_ordered);

  // 2. 輸入亂序 FZURO -> 未開重排時必須無法切分（或者長度無法推進到 5）
  SyllableGraph graph_disordered;
  int farthest_disordered = syllabifier.BuildSyllableGraph(
      "FZURO", *loaded_prism_, &graph_disordered);
  EXPECT_NE(5, farthest_disordered);
}

// Tests that the spelling cache is used correctly when the same spelling_id
// is encountered from multiple BFS vertices (ambiguous parsing).
// Builds a local syllabary that includes "n" as a standalone syllable to
// exercise the deeper ambiguous path: a + n + a + na.
// "a" spelling_id is queried at vertices 0 (miss) and 2,4 (hit).
// "n" spelling_id is queried at vertices 1 (miss) and 3 (hit).
TEST_F(RimeSyllabifierTest, AmbiguousCacheHit) {
  rime::vector<rime::string> syllables;
  syllables.push_back("a");
  syllables.push_back("an");
  syllables.push_back("n");
  syllables.push_back("na");
  std::sort(syllables.begin(), syllables.end());

  rime::map<rime::string, rime::SyllableId> sid;
  for (size_t i = 0; i < syllables.size(); ++i) {
    sid[syllables[i]] = i;
  }

  rime::path file_path("syllabifier_cache_test.bin");
  auto test_prism = rime::New<rime::Prism>(file_path);
  test_prism->Remove();
  rime::set<rime::string> keyset;
  std::copy(syllables.begin(), syllables.end(),
            std::inserter(keyset, keyset.begin()));
  EXPECT_TRUE(test_prism->Build(keyset));

  rime::Syllabifier s;
  rime::SyllableGraph g;
  const rime::string input("anana");
  int result = s.BuildSyllableGraph(input, *test_prism, &g);
  EXPECT_EQ(input.length(), result);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_EQ(input.length() + 1, g.vertices.size());

  // Verify key vertices and edges for the a-n-a-na path
  EXPECT_FALSE(g.edges.end() == g.edges.find(0));
  EXPECT_FALSE(g.edges.end() == g.edges.find(1));
  EXPECT_FALSE(g.edges.end() == g.edges.find(2));
  EXPECT_FALSE(g.vertices.end() == g.vertices.find(1));
  EXPECT_FALSE(g.vertices.end() == g.vertices.find(3));

  // a@0→1
  auto& e0 = g.edges[0];
  EXPECT_FALSE(e0.end() == e0.find(1));
  EXPECT_FALSE(e0[1].end() == e0[1].find(sid["a"]));
  // n@1→2
  auto& e1 = g.edges[1];
  EXPECT_FALSE(e1.end() == e1.find(2));
  EXPECT_FALSE(e1[2].end() == e1[2].find(sid["n"]));
  // a@2→3
  auto& e2 = g.edges[2];
  EXPECT_FALSE(e2.end() == e2.find(3));
  EXPECT_FALSE(e2[3].end() == e2[3].find(sid["a"]));
  // na@3→5
  auto& e3 = g.edges[3];
  EXPECT_FALSE(e3.end() == e3.find(5));
  EXPECT_FALSE(e3[5].end() == e3[5].find(sid["na"]));

  test_prism->Remove();
}

// Test with spelling algebra (Script) to exercise the full cached
// QuerySpelling path with a non-null spelling_map.
TEST_F(RimeSyllabifierTest, SpellingAlgebraCache) {
  rime::set<rime::string> keyset;
  keyset.insert("zhi");
  keyset.insert("li");
  keyset.insert("shi");

  rime::Script script;
  {
    rime::Spelling s("zhi");
    s.properties.type = rime::kNormalSpelling;
    script["zhi"].push_back(s);
  }
  {
    rime::Spelling s("zhi");
    s.properties.type = rime::kAbbreviation;
    script["zh"].push_back(s);
  }
  {
    rime::Spelling s("li");
    s.properties.type = rime::kNormalSpelling;
    script["li"].push_back(s);
  }
  {
    rime::Spelling s("shi");
    s.properties.type = rime::kNormalSpelling;
    script["shi"].push_back(s);
  }

  rime::path file_path("syllabifier_algebra_test.bin");
  auto test_prism = rime::New<rime::Prism>(file_path);
  test_prism->Remove();
  EXPECT_TRUE(test_prism->Build(keyset, &script));
  EXPECT_TRUE(test_prism->Save());
  EXPECT_TRUE(test_prism->Load());

  rime::Syllabifier s;
  rime::SyllableGraph g;
  const rime::string input("zhili");
  int result = s.BuildSyllableGraph(input, *test_prism, &g);
  EXPECT_EQ(input.length(), result);
  EXPECT_EQ(input.length(), g.interpreted_length);
  EXPECT_FALSE(g.vertices.end() == g.vertices.find(3));
  EXPECT_FALSE(g.vertices.end() == g.vertices.find(5));
  EXPECT_FALSE(g.edges.end() == g.edges.find(3));
  EXPECT_FALSE(g.edges[3].end() == g.edges[3].find(5));

  test_prism->Remove();
}
