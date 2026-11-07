#include "lexical_index.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <cmath>

using namespace pacificdb;
template<class F> void fails(F function, const std::string& expected) {
    bool failed = false;
    try { function(); } catch (const std::exception& e) { failed = std::string(e.what()).find(expected) != std::string::npos; }
    assert(failed);
}
// Independent exhaustive BM25 reference: ASCII fixture text is whitespace-delimited.
std::vector<std::pair<std::string,double>> reference(const std::map<std::string,std::string>& corpus,
    const std::string& query, bool all = false) {
    std::map<std::string,std::map<std::string,size_t>> counts;
    std::map<std::string,size_t> lengths, frequency;
    size_t total = 0;
    for (const auto& row : corpus) {
        std::istringstream words(row.second); std::string word;
        while (words >> word) { ++counts[row.first][word]; ++lengths[row.first]; ++total; }
        for (const auto& term : counts[row.first]) ++frequency[term.first];
    }
    std::istringstream words(query); std::set<std::string> terms; std::string word;
    while (words >> word) terms.insert(word);
    std::vector<std::pair<std::string,double>> result;
    for (const auto& row : corpus) {
        double score = 0; size_t matched = 0;
        for (const auto& term : terms) {
            const auto tf = counts[row.first][term];
            if (!tf) continue;
            ++matched;
            const double idf = std::log((corpus.size()+1.)/(frequency[term]+.5));
            const double length = double(lengths[row.first])*corpus.size()/total;
            score += idf*2.2/(1.+1.2*(.25+.75*length)/tf);
        }
        if (matched && (!all || matched == terms.size())) result.emplace_back(row.first, score);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    });
    if (result.size() > 10) result.resize(10); return result;
}
void compare(const LexicalIndex& index, const std::map<std::string,std::string>& corpus,
             const std::string& query, bool all = false) {
    const auto expected = reference(corpus, query, all);
    const auto actual = index.search(query, 10, all); assert(expected.size() == actual.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        assert(actual[i].first == expected[i].first);
        assert(std::abs(actual[i].second-expected[i].second) < 1e-12);
    }
}
int main() {
    assert(LexicalIndex::tokenize("Alpha,beta ALPHA! 123", 100, 10) ==
        std::vector<std::string>({"alpha", "beta", "alpha", "123"}));
    fails([] { LexicalIndex::tokenize("too many words", 100, 2); }, "token limit");
    LexicalIndex index;
    std::map<std::string,std::string> corpus{{"a","alpha alpha beta"}, {"b","alpha gamma"},
        {"c","beta delta delta delta"}, {"d",""}};
    for (const auto& row : corpus) assert(index.upsert(row.first,row.second));
    compare(index,corpus,"alpha beta"); compare(index,corpus,"alpha beta",true);
    assert(index.search("unknown",10).empty()); assert(index.search("alpha unknown",10,true).empty());
    assert(index.search("ALPHA",10) == index.search("alpha alpha",10));
    assert(index.erase("b")); corpus.erase("b"); assert(!index.erase("b"));
    assert(index.upsert("a","omega")); corpus["a"]="omega";
    compare(index,corpus,"alpha beta omega");
    assert(index.upsert("b","alpha")); corpus["b"]="alpha";
    compare(index,corpus,"alpha beta omega");
    size_t polls=0;
    fails([&] { index.search("alpha beta omega",10,false,[&] {
        if (++polls==4) throw std::runtime_error("cancelled");
    }); },"cancelled");
    assert(index.stats().at("healthy")==true);
    LexicalConfig small; small.maxCandidates=1;
    LexicalIndex limited(small); limited.upsert("a","term");limited.upsert("b","term");
    fails([&] { limited.search("term",10); },"candidate_limit");
    assert(limited.stats().at("healthy")==true);
    std::mt19937 rng(1701);
    for (int n=0;n<500;++n) {
        std::string text; const auto length=3+rng()%20;
        for(size_t k=0;k<length;++k) text+=" word"+std::to_string(rng()%32);
        corpus["doc"+std::to_string(n)]=text; index.upsert("doc"+std::to_string(n),text);
    }
    for (int n=0;n<100;++n) {
        const auto query="word"+std::to_string(rng()%32)+" word"+std::to_string(rng()%32);
        compare(index,corpus,query); compare(index,corpus,query,true);
    }
    auto budget=std::make_shared<VectorAllocationBudget>(4096);
    {
        LexicalIndex bounded({},budget);
        bool exhausted=false;
        try { for(int i=0;i<1000;++i)bounded.upsert(std::to_string(i),"alpha beta gamma delta"); }
        catch(const VectorMemoryLimit&){exhausted=true;}
        assert(exhausted && bounded.stats().at("healthy")==false);
        fails([&]{bounded.search("alpha",10);},"unhealthy");
    }
    assert(budget->used()==0 && budget->peak()<=4096);
    LexicalIndex concurrent;
    for(int i=0;i<100;++i)concurrent.upsert(std::to_string(i),"alpha beta");
    std::thread writer([&]{for(int i=0;i<100;++i){concurrent.erase(std::to_string(i));concurrent.upsert(std::to_string(i),"gamma alpha");}});
    std::vector<std::thread> readers;
    for(int r=0;r<4;++r)readers.emplace_back([&]{for(int i=0;i<100;++i){
        const auto found=concurrent.search("alpha",10); assert(found.size()==10);
        std::set<std::string> unique;for(const auto& hit:found)assert(unique.insert(hit.first).second && std::isfinite(hit.second));
    }});
    writer.join();for(auto& reader:readers)reader.join();
    assert(concurrent.stats().at("live_records")==100);
    const std::vector<std::pair<std::string,double>> vectorRanks{{"a",.9},{"b",.8}}, lexicalRanks{{"b",5},{"c",4}};
    const auto fusion=reciprocalRankFusion(vectorRanks,lexicalRanks,3);
    assert(fusion.size()==3 && fusion[0].first=="b" && fusion[1].first=="a" && fusion[2].first=="c");
    assert(std::abs(fusion[0].second-(1./61+1./62))<1e-15);
    assert(reciprocalRankFusion(vectorRanks,lexicalRanks,3,0,1)[0].first=="b");
    fails([&]{reciprocalRankFusion({{"a",1},{"a",0}},lexicalRanks,3);},"duplicate");
    fails([&]{reciprocalRankFusion(vectorRanks,lexicalRanks,3,0,0);},"parameters");
    std::cout<<"LEXICAL_BM25_AND_RRF_PASS\n";
}
