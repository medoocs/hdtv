#include <algorithm>
#include <fstream>
#include <vector>
#include <string>
#include <set>

#include <cstdio>
#include <cmath>
#include <cstring>
/* 
*** STO NEVALJA ***

1. "weights" su nam krivo pretpostavljeni da trebaju biti 1
    - smanje se za rows koji kad splittamo na atributu s missing value
2. hellingera krivo izracunavamo jer smo retardirani
    - za diskretne slucajeve je kompletno krivo (kinda emulira continuous)
    - za "continumous" radi samo za 2 klase
    - NOTE: attrClassCnts[][] pribrojava sve weights u rows za trenutacni atribut gdje
            prvi [] odgovara vrijednosti atributa, a drugi klasi na kraju rowa

3. krucijalno, sve je presporo, a C4.5 je mozda najsporija stvar ikad napisana

4. dosta toga je c/p, moglo bi se nekako generalizirati da CART i C4.5 budu bazne podjele,
    a racunanje split gain-a da bude neovisno o tome

5. seemingly Information Gain Ratio konzistentno polucuje losijim rezultatima od Information Gain
    - ?? nema smisli

*/

// TODO: sve osim stats u log file or sth | ultra ez
// TODO: handle missing data | medium-zajebano
// TODO: implementiraj multi-class hellinger distance za CART algoritam 
// TODO: dodaj grid search za hiperparametre | medium
// TODO: dodaj mogucnost spremanja stvorenog stabla | kinda ez-medium
// TODO: fixati apsolutno sve da nije ovako fugly | zajebano

// lol
// TODO: multi-core | samo kinda zajebano
// TODO: gpu????? | poprilicno zajebano
// TODO: hellinger net????? lol | ultra zajebano delaj u pajtonima

#define DEBUG(text) fprintf(stderr, "%d: %s\n", __LINE__, text);

//#define PROFILING
#ifdef PROFILING
#include "profiler.hpp"
#define BEGIN_SESSION(name) Instrumentor::get().beginSession(name);
#define END_SESSION() Instrumentor::get().endSession();
#define PROFILE_SCOPE(name) Timer timer##__LINE__(name);
#define PROFILE_FUNCTION PROFILE_SCOPE(__FUNCTION__)
#else
#define BEGIN_SESSION(name)
#define END_SESSION()
#define PROFILE_SCOPE(name)
#define PROFILE_FUNCTION
#endif

union Z {
    int i;
    float f;
    Z() {}
    Z(int x): i(x) {}
    Z(float x): f(x) {}
};

using Row = std::vector<Z>;
using Rows = std::vector<Row>;

int minorityClass; // TODO: get rid of these globals, legitimately just send them to everything that uses them
std::vector<int> totalHist;
std::vector<std::string> classes;
std::vector<std::string> attrNames;
std::vector<std::vector<std::string>> attrValues; // att[0] == {"b", "o", "x"}
std::vector<bool> isContinuous;
std::vector<float> mins, maxs;
std::vector<int> uniqueValueCount;

namespace Utils {
    const float epsilon = 1e-6;
    bool eq(float x, float y) {
        return fabs(x-y) <= epsilon;
    }

    Rows filter(const Rows &rows, int label) {
        // filter only for rows with label == `label`
        Rows new_rows;
        for (const auto &row: rows) {
            if (row.back().i == label) {
                new_rows.push_back(row);
            }
        }
        return new_rows;
    }

    int count(const Rows &rows, int label) {
        // count only for rows with label == `label`
        int ret = 0;
        for (const auto &row: rows) {
            if (row.back().i == label) {
                ++ret;
            }
        }
        return ret;
    }

    inline float sqr(float x) {
        return x*x;
    }

    void trim(std::string &s) {
        int start = 0, end = s.size()-1;
        while (s[start] == ' ') ++start;
        while (s[end] == ' ') --end;
        // "   abcdefgh   "
        //     ^      ^
        s = s.substr(start, end-start+1);
    }

    template<typename T>
    T accumulate(std::vector<T> v, T init = 0) {
        for (const auto &e: v) {
            init += e;
        }
        return init;
    }

    float log(float x) {
        if (eq(x, 0)) {
            return 0;
        } else {
            return log2f(x);
        }
    }
}

struct Question {
    int column;
    Z value; // TODO type pending // *mozda* nas nije briga

    int match(const Row &example, bool c45 = false) const { // a0: 5, a1: "burek", a2: 6
        // Compare the feature value in an example to the
        // feature value in this question.
        if (isContinuous[column]) {
            return example[column].f >= value.f;
        }
        if (c45) { // returns the index of the value in attrValues[column]
            return example[column].i;
        }
        return example[column].i == (int)value.i;
    }
};

std::vector<int> class_histogram(const Rows &rows) {
    /*
    count number of rows per class
    */
    std::vector<int> histogram(classes.size(), 0);
    for (const auto &row: rows) {
        int label = row.back().i;
        ++histogram[label];
    }
    return histogram;
}

struct Node {
    bool isLeaf = false;
    std::vector<Node *> children;
    Question question;
    std::vector<int> predictions;
};

Node *DecisionNodeContinuous(Node *l, Node *r, const Question &q) {
    Node *node = new Node();
    node->question = q;
    node->children = {l, r};
    return node;
}

Node *Leaf(const Rows &rows) {
    Node *node = new Node();
    node->isLeaf = true;
    node->predictions = class_histogram(rows);
    return node;
}

float gini(const Rows &rows) {
    /*
    Calculate the Gini Impurity for a list of rows.

    There are a few different ways to do this, I thought this one was
    the most concise. See:
    https://en.wikipedia.org/wiki/Decision_tree_learning#Gini_impurity
    */
    auto histogram = class_histogram(rows);
    float impurity = 1;
    for (int label: histogram) {
        float probability = 1. * label / rows.size();
        impurity -= probability*probability;
    }
    return impurity;
}

float new_gini(const std::vector<int> &histogram) {
    float impurity = 1;
    for (int i = 0; i < (int)histogram.size()-1; ++i) { // -1 jer je zadnji broj redova
        int label = histogram[i];
        float probability = 1. * label / histogram.back();
        impurity -= probability*probability;
    }
    return impurity;
}

float entropy(const Rows &rows) {
    if (rows.size() == 0) return 0;
    auto num_classes = class_histogram(rows);
    float ent = 0;
    for (int num: num_classes) {
        float x = 1.*num/rows.size();
        ent += x*Utils::log(x);
    }
    return -ent;
}

float new_entropy(const std::vector<int> &num_classes) {
    if (num_classes.back() == 0) return 0;
    float ent = 0;
    for (int i = 0; i < (int)num_classes.size()-1; ++i) { // -1 jer je zadnji broj redova
        int num = num_classes[i];
        float x = 1.*num/num_classes.back();
        ent += x*Utils::log(x);
    }
    return -ent;
}

float informationGain(const Rows &rows, const std::vector<Rows> &splits, float beforeEntropy) {
    float afterEntropy = 0;
    for (int i = 0; i < (int)splits.size(); ++i) {
        float weight = 1. * splits[i].size() / rows.size();
        afterEntropy += weight * entropy(splits[i]);
    }
    return beforeEntropy - afterEntropy;
}

float new_informationGain(int rowSize, const std::vector<std::vector<int>> &splits, float beforeEntropy) {
    float afterEntropy = 0;
    for (const auto &split: splits) {
        float weight = 1. * split.back() / rowSize;
        afterEntropy += weight * new_entropy(split);
    }
    return beforeEntropy - afterEntropy;
}

float intrinsicValue(const Rows &rows, const std::vector<Rows> &splits) {
    float iv = 0;
    for (const auto& split: splits) {
        float weight = 1. * split.size() / rows.size();
        iv += weight * Utils::log(weight);
    }
    return -iv;
}

float informationGainRatio(const Rows &rows, const std::vector<Rows> &splits, float beforeEntropy) {
    return informationGain(rows, splits, beforeEntropy) / intrinsicValue(rows, splits);
}

float new_informationGainRatio(int rowSize, const std::vector<std::vector<int>> &splits, float beforeEntropy) {
    float afterEntropy = 0;
    float iv = 0;
    for (const auto &split: splits) {
        float weight = 1. * split.back() / rowSize;
        afterEntropy += weight * new_entropy(split);
        iv += weight * Utils::log(weight);
    }
    return (beforeEntropy - afterEntropy) / -iv;
}

std::set<float> feature_values(int col, const Rows &rows) {
    // TODO: ne moze ostati float, ovisit ce o tipu attr
    // *mozda* nas to nije briga?
    std::set<float> values;
    for (const auto &row: rows) {
        if (isContinuous[col])
            values.insert(row[col].f);
        else
            values.insert(row[col].i);
    }
    return values;
}

// u rows koliko ima labela == minorityClass takvih da je question.match(row)

void partition_class_histogram(const Rows &rows, const Question &question, bool c45, std::vector<std::vector<int>> &splits) {
    for (const auto &row: rows) {
        ++splits[question.match(row, c45)][row.back().i]; // "false" should depend on c45
        ++splits[question.match(row, c45)].back();
    }
}

// partition continuous
void partition(const Rows &rows, const Question &question, bool c45, Rows &true_rows, Rows &false_rows) {
    for (const auto &row: rows) {
        if (question.match(row, c45)) {
            true_rows.push_back(row);
        } else {
            false_rows.push_back(row);
        }
    }
}

// partition discrete
void partition(const Rows &rows, const Question &question, bool c45, std::vector<Rows> &splits) {
    splits.resize(attrValues[question.column].size());
    for (const auto &row: rows) {
        splits[question.match(row, c45)].push_back(row);
    }
}

/* <google indian> */
float gini_gain(const Rows &left, const Rows &right,
                float current_uncertainty) {
    float p = 1. * left.size() / (left.size() + right.size());    
    return current_uncertainty - p*gini(left) - (1-p)*gini(right);
}

float new_gini_gain(const std::vector<std::vector<int>> &splits,
                    float current_uncertainty) {
    float p = 1. * splits[0].back() / (splits[0].back() + splits[1].back());
    return current_uncertainty - p*new_gini(splits[0]) - (1-p)*new_gini(splits[1]);
}

void split_continuous(const Rows &rows, int column, float current_uncertainty,
                      float &best_gain, Question &best_question,
                      int max_buckets, bool c45) {
    std::set<float> values;
    if (max_buckets != 0) {
        float min = mins[column], max = maxs[column];
        float step = (max-min)/max_buckets;
        for (int bucket = 0; bucket <= max_buckets; ++bucket) {
            values.insert(bucket*step+min);
        }
    } else {
        values = feature_values(column, rows);
    }
    int size = 2;
    std::vector<std::vector<int>> splits(size, std::vector<int>(classes.size()+1, 0)); // +1 za broj redova u tom splitu
    for (float value: values) {
        Question question = {column, value};
        /*
        Rows true_rows, false_rows;
        partition(rows, question, true_rows, false_rows);
        if (true_rows.size() == 0 || false_rows.size() == 0) {
            continue;
        }
        float gain = gini_gain(true_rows, false_rows, current_uncertainty); //*/
        //*
        for (auto &split: splits) for (int &e: split) e = 0;
        partition_class_histogram(rows, question, c45, splits);
        if (splits[0].back() == 0 || splits[1].back() == 0) continue;
        float gain = new_gini_gain(splits, current_uncertainty);//*/
        if (gain > best_gain) {
            best_gain = gain;
            best_question = question;
        }
    }
}

void split_discrete(const Rows &rows, int column, float current_uncertainty,
                    float &best_gain, Question &best_question,
                    bool c45) {
    int size = attrValues[column].size();
    std::vector<std::vector<int>> splits(size, std::vector<int>(classes.size()+1, 0)); // +1 za broj redova u tom splitu
    for (int value = 0; value < (int)attrValues[column].size(); ++value) {
        Question question = {column, value};
        Rows true_rows, false_rows;
        //if (true_rows.size() == 0 || false_rows.size() == 0) {
        //    continue;
        //}
        //float gain = gini_gain(true_rows, false_rows, current_uncertainty);
        for (auto &split: splits) for (int &e: split) e = 0;
        partition_class_histogram(rows, question, c45, splits);
        if (splits[0].back() == 0 || splits[1].back() == 0) continue;
        float gain = new_gini_gain(splits, current_uncertainty);
        if (gain > best_gain) {
            best_gain = gain;
            best_question = question;
        }
    }
}

void find_best_split_indian(Rows &rows, float &best_gain, 
                            Question &best_question, int max_buckets, bool c45) {
    best_gain = 0;
    float current_uncertainty = gini(rows);
    int n_features = rows[0].size() - 1;
    for (int column = 0; column < n_features; ++column) {
        if (isContinuous[column]) {
            split_continuous(rows, column, current_uncertainty, best_gain, best_question, max_buckets, c45);
        } else {
            split_discrete(rows, column, current_uncertainty, best_gain, best_question, c45);
        }
    }
}
/* </google indian> */

/* <hellinger> */
float hellinger_distance(int lsize, int rsize, float tp, float tfvp, float tfwp) {
    float tfvn = lsize - tfvp;
    float tfwn = rsize - tfwp;
    float tn = (lsize + rsize) - tp;
    //printf("%f %f %f %f %f %f\n", tfvn, tfwn, tn, tfvp, tfwp, tp);
    return Utils::sqr(std::sqrt(tfvp/tp) - std::sqrt(tfvn/tn))
         + Utils::sqr(std::sqrt(tfwp/tp) - std::sqrt(tfwn/tn));
}

void hellinger_split_continuous(const Rows &rows, int column, int tp,
                                float &best_gain, Question &best_question,
                                int max_buckets, bool c45) {
    std::set<float> values;
    if (max_buckets != 0) {
        float min = mins[column], max = maxs[column];
        float step = (max-min)/max_buckets;
        for (int bucket = 0; bucket <= max_buckets; ++bucket) {
            values.insert(bucket*step+min);
        }
    } else {
        values = feature_values(column, rows);
    }
    for (float value: values) {
        Question question = {column, value};
        int lsize = 0, rsize = 0;
        int tfvp = 0, tfwp = 0;
        for (const auto &row: rows) {
            if (question.match(row, c45)) {
                ++lsize;
                if (row.back().i == minorityClass) ++tfvp;
            } else {
                ++rsize;
                if (row.back().i == minorityClass) ++tfwp;
            }
        }
        if (lsize == 0 || rsize == 0) {
            continue;
        }
        float gain = hellinger_distance(lsize, rsize, tp, tfvp, tfwp);
        //printf("%f\n", gain);
        if (gain > best_gain) {
            best_gain = gain;
            best_question = question;
        }
    }
}

void hellinger_split_discrete(const Rows &rows, int column, int tp,
                              float &best_gain, Question &best_question,
                              bool c45) {
    //std::vector<std::vector<float>> attrClassCnts(attrValues[column].size(), std::vector<float>(classes.size()));
    /*for (const auto &row: rows) {
        attrClassCnts[row[column].i][row.back().i] += 1.0f; // should be weight
    }*/
    for (int value = 0; value < (int)attrValues[column].size(); ++value) {
        Question question = {column, value};
        int lsize = 0, rsize = 0;
        int tfvp = 0, tfwp = 0;
        for (const auto &row: rows) {
            if (question.match(row, c45)) {
                ++lsize;
                if (row.back().i == minorityClass) ++tfvp;
            } else {
                ++rsize;
                if (row.back().i == minorityClass) ++tfwp;
            }
        }
        if (lsize == 0 || rsize == 0) {
            continue;
        }
        float gain = hellinger_distance(lsize, rsize, tp, tfvp, tfwp);
        if (gain > best_gain) {
            best_gain = gain;
            best_question = question;
        }
    }
}

void new_hellinger_split_continuous(Rows &rows, int column,
                                float &best_gain, Question &best_question) {
    std::sort(rows.begin(), rows.end(), [column](const Row &a, const Row &b){
        return a[column].f < b[column].f;
    });
    std::vector<float> leftFreq(classes.size()), rightFreq(classes.size());
    for (const auto &row: rows) {
        rightFreq[row.back().i] += 1.0f; // should be the weight of the row
    }
    // for now we just treat the last row as a whole row
    // but once we start handling missing data and weighting rows
    // properly, the actual constraint should be dependent on
    // the amount of missing data, i.e. no branch can contain
    // only missing data
    int rsize = rows.size(), lsize = 0;
    for (int i = 1; i < (int)rows.size()-1; ++i) {
        const auto &row = rows[i];
        float value = rows[i-1][column].f;
        rightFreq[row.back().i] -= 1.0f; // should be the weight of the row
        if (Utils::eq(rightFreq[row.back().i], 0)) {
            rightFreq[row.back().i] = 0.0f;
        }
        --rsize;
        leftFreq[rows[i-1].back().i] += 1.0f; // should be the weight of the row
        ++lsize;
        if (lsize == 0) continue;
        if (rsize == 0) break;
        if (Utils::eq(row[column].f, value)) continue;
        float dblsum = 0;
        int nPairs = 0;
        for (int c1 = 0; c1 < (int)classes.size()-1; ++c1) {
            for (int c2 = c1+1; c2 < (int)classes.size(); ++c2) {
                //find size of X1 and X2
                float nX1 = rightFreq[c1] + leftFreq[c1];
                float nX2 = rightFreq[c2] + leftFreq[c2];
                //since attribute values can only be "left" or "right" only two terms in summation
                float radicand = Utils::sqr(sqrt(1.*rightFreq[c2]/nX2) - sqrt(1.*rightFreq[c1]/nX1))+
                                 Utils::sqr(sqrt(1.*leftFreq[c2]/nX2) - sqrt(1.*leftFreq[c1]/nX1));

                dblsum += sqrt(radicand);
                nPairs++;
            }
        }
        float gain = dblsum/nPairs;
        if (gain > best_gain) {
            best_gain = gain;
            best_question = {column, row[column]};
        }
    }
}

void new_hellinger_split_discrete(const Rows &rows, int column,
                                  float &best_gain, Question &best_question) {
    std::vector<std::vector<float>> attrClassCnts(attrValues[column].size(), std::vector<float>(classes.size()));
    for (const auto &row: rows) {
        attrClassCnts[row[column].i][row.back().i] += 1.0f; // should be weight
    }
    int legitChildren = 0;
    for (int i = 0; i < (int)attrValues[column].size(); ++i) {
        float size = 0.0f;
        for (int j = 0; j < (int)classes.size(); ++j) {
            size += attrClassCnts[i][j];
        }
        if (size > 0) {
            ++legitChildren;
        }
    }
    if (legitChildren < 2) {
        // no branching, don't process or update gain/question
        return;
    }

    float dblsum = 0;
	int nPairs = 0;
    for (int c1 = 0; c1 < (int)classes.size()-1; ++c1) {
        for (int c2 = c1+1; c2 < (int)classes.size(); ++c2) {
            float nX1 = 0, nX2 = 0;
			for(int k = 0; k < (int)attrValues[column].size(); ++k){
				nX1+=attrClassCnts[k][c1];
				nX2+=attrClassCnts[k][c2];
			}
			float fradicand = 0;
			//find size of X1j and X2j -- attrClassCnts[j][0], [j][1]
			//sum up the radicand 
			for(int k = 0; k < (int)attrValues[column].size(); ++k){
				float nX1j = attrClassCnts[k][c1];
				float nX2j = attrClassCnts[k][c2];    

				fradicand += Utils::sqr((sqrt(1.*nX1j/nX1) - sqrt(1.*nX2j/nX2)));
			}
			dblsum += sqrt(fradicand);
			nPairs++;
        }
    }
    float gain = dblsum/nPairs;
    if (gain > best_gain) {
        best_gain = gain;
        best_question = {column, 0};
    }
}

void find_best_split_hellinger(Rows &rows,
                               float &best_gain, Question &best_question,
                               int max_buckets, bool c45) {
    best_gain = 0;
    float tp = Utils::count(rows, minorityClass);
    int n_features = rows[0].size() - 1;
    for (int column = 0; column < n_features; ++column) {
        if (isContinuous[column]) {
            hellinger_split_continuous(rows, column, tp, best_gain, best_question, max_buckets, c45);
        } else {
            hellinger_split_discrete(rows, column, tp, best_gain, best_question, c45);
        }
    }
}
/* </hellinger> */

void IG_split_continuous(const Rows &rows, int column, float beforeEntropy,
                         float &best_gain, Question &best_question, int max_buckets, bool c45, bool IGR) { // IGR should be templated
    std::set<float> values;
    if (max_buckets != 0) {
        float min = mins[column], max = maxs[column];
        float step = (max-min)/max_buckets;
        for (int bucket = 0; bucket <= max_buckets; ++bucket) {
            values.insert(bucket*step+min);
        }
    } else {
        values = feature_values(column, rows);
    }
    int size = 2;
    std::vector<std::vector<int>> splits(size, std::vector<int>(classes.size()+1, 0)); // +1 za broj redova u tom splitu
    for (float value: values) {
        Question question = {column, value};
        /*
        Rows true_rows, false_rows;
        partition(rows, question, c45, true_rows, false_rows);
        float gain = informationGain(rows, {false_rows, true_rows}, beforeEntropy);//*/
        for (auto &split: splits) for (int &e: split) e = 0;
        partition_class_histogram(rows, question, c45, splits);
        float gain = (IGR?new_informationGainRatio:new_informationGain)(rows.size(), splits, beforeEntropy);
        if (gain > best_gain) {
            best_gain = gain;
            best_question = question;
        }
    }
}

void IG_split_discrete(const Rows &rows, int column, float beforeEntropy,
                       float &best_gain, Question &best_question, bool c45, bool IGR) { // IGR should be a template argument
    int size = attrValues[column].size();
    std::vector<std::vector<int>> splits(size, std::vector<int>(classes.size()+1, 0)); // +1 za broj redova u tom splitu
    for (int value = 0; value < (int)attrValues[column].size(); ++value) {
        Question question = {column, value}; // value is unused
        /*
        std::vector<Rows> splits;
        partition(rows, question, c45, splits);
        float gain = informationGain(rows, splits, beforeEntropy);//*/
        for (auto &split: splits) for (int &e: split) e = 0;
        partition_class_histogram(rows, question, c45, splits);
        float gain = (IGR?new_informationGainRatio:new_informationGain)(rows.size(), splits, beforeEntropy);
        if (gain > best_gain) {
            best_gain = gain;
            best_question = question;
        }
    }
}

void find_best_split_C45_IG(Rows &rows,
                            float &best_gain, Question &best_question,
                            int max_buckets, bool c45) {
    best_gain = 0;
    float beforeEntropy = entropy(rows);
    int n_features = rows[0].size() - 1;
    for (int column = 0; column < n_features; ++column) {
        if (isContinuous[column]) {
            IG_split_continuous(rows, column, beforeEntropy, best_gain, best_question, max_buckets, c45, false);
        } else {
            IG_split_discrete(rows, column, beforeEntropy, best_gain, best_question, c45, false);
        }
    }
}

void find_best_split_C45_IGR(Rows &rows,
                             float &best_gain, Question &best_question,
                             int max_buckets, bool c45) {
    best_gain = 0;
    float beforeEntropy = entropy(rows);
    int n_features = rows[0].size() - 1;
    for (int column = 0; column < n_features; ++column) {
        if (isContinuous[column]) {
            IG_split_continuous(rows, column, beforeEntropy, best_gain, best_question, max_buckets, c45, true);
        } else {
            IG_split_discrete(rows, column, beforeEntropy, best_gain, best_question, c45, true);
        }
    }
}

void find_best_split_C45_hellinger(Rows &rows,
                                   float &best_gain, Question &best_question,
                                   int, bool) {
    best_gain = 0;
    int n_features = rows[0].size() - 1;
    for (int column = 0; column < n_features; ++column) {
        if (isContinuous[column]) {
            new_hellinger_split_continuous(rows, column, best_gain, best_question);
        } else {
            new_hellinger_split_discrete(rows, column, best_gain, best_question);
        }
    }
}

template<void (*split_function)(Rows &, float &, Question &, int, bool)>
Node *build_CART_tree(Rows &rows, int depth, int max_buckets) {
    float gain; 
    Question question;
    if (depth == 0) {
        return Leaf(rows);
    }
    split_function(rows, gain, question, max_buckets, false);
    if (Utils::eq(gain, 0)) {
        return Leaf(rows);
    }
    Rows true_rows, false_rows;
    partition(rows, question, false, true_rows, false_rows);

    Node *true_branch = build_CART_tree<split_function>(true_rows, depth-1, max_buckets);
    Node *false_branch = build_CART_tree<split_function>(false_rows, depth-1, max_buckets);
    return DecisionNodeContinuous(false_branch, true_branch, question);
}

template<void (*split_function)(Rows &, float &, Question &, int, bool)>
Node *build_C45_tree(Rows &rows, int depth, int max_buckets) {
    float gain; 
    Question question;
    if (depth == 0) {
        return Leaf(rows);
    }
    split_function(rows, gain, question, max_buckets, true);
    if (Utils::eq(gain, 0)) {
        return Leaf(rows);
    }
    std::vector<Rows> splits;
    if (isContinuous[question.column]) {
        splits.resize(2);
        partition(rows, question, true, splits[1], splits[0]);
    } else {
        partition(rows, question, true, splits);
    }

    Node *node = new Node();
    node->question = question;
    for (auto &child_rows: splits) {
        if (child_rows.size() == 0) {
            node->children.push_back(Leaf(rows));
        } else {
            node->children.push_back(build_C45_tree<split_function>(child_rows, depth-1, max_buckets));
        }
    }
    return node;
}

void print_tree(Node *node, const std::string &spacing="") {
    if (node->isLeaf) {
        printf("%sPredict {", spacing.c_str());
        bool comma = false;
        for (int i = 0; i < (int)classes.size(); ++i) { // should always be 2
            if (node->predictions[i] == 0) continue;
            printf("%s%s: %d", (comma?", ":""), classes[i].c_str(), node->predictions[i]);
            comma = true;
        }
        printf("}\n");
        return;
    }
    printf("%s", spacing.c_str());
    printf("Is %s ", attrNames[node->question.column].c_str());
    if (isContinuous[node->question.column]) {
        printf(">= %.3f?\n", node->question.value.f);
    } else {
        printf("== %s?\n", attrValues[node->question.column][(int)node->question.value.i].c_str());
    }

    for (int child = 0; child < (int)node->children.size(); ++child) {
        printf("%s--> %d:\n", spacing.c_str(), child); // TODO: store more info about the node for better prints
        print_tree(node->children[child], spacing+"  ");
    }
}

std::vector<float> classify(const Row &row, Node *node, bool c45 = false) {
    if (node->isLeaf) {
        const float total = Utils::accumulate(node->predictions);
        std::vector<float> probs;
        for (int e: node->predictions) {
            // LAPLACE
            probs.push_back(1.*(e+1)/(total + classes.size()));
        }
        return probs;
    }
    return classify(row, node->children[node->question.match(row, c45)], c45);
}

void print_leaf(const std::vector<int> &counts, int total) {
    printf("{");
    bool comma = false;
    for (int i = 0; i < (int)classes.size(); ++i) {
        float p = 1.*counts[i]/total*100;
        if (counts[i] == 0) continue;
        printf("%s%s: %.2f%%", (comma?", ":""), classes[i].c_str(), p);
        comma = true;
    }
    printf("}\n");
}

std::vector<std::string> parseLine(const std::string &line, char delimiter=',') {
    std::vector<std::string> splits;
    std::string cur;
    for (char c: line) {
        if (c == delimiter) {
            Utils::trim(cur);
            splits.push_back(cur);
            cur = "";
        } else {
            cur.push_back(c);
        }
    }
    Utils::trim(cur);
    if (!cur.empty()) {
        splits.push_back(cur);
    }
    return splits;
}

void makeDatastructure(const std::string &filePath) {
    std::ifstream fin(filePath);
    std::string line;
    bool getClasses = true;
    bool getAttrs = false;
    for (int lineno = 0; getline(fin, line); ++lineno) {
        Utils::trim(line);
        if (line.size() == 0) continue;
        if (line.back() == '.') line.pop_back();
        if (line == "|classes") {
            getClasses = true;
        } else if (getClasses) {
            classes = parseLine(line);
            if (classes.size() == 1) {
                fprintf(stderr, "%s\n%s\nWhat are you doing with just one class lol\n", line.c_str(), classes[0].c_str());
                exit(1);
            }
            getClasses = false;
            getAttrs = true;
        } else if (line == "|attributes") { // maybe unnecessary?
            getAttrs = true;
        } else if (getAttrs) {
            int colon = line.find(':');
            std::string attrName = line.substr(0, colon);
            attrNames.push_back(attrName);
            line = line.substr(colon+1);
            attrValues.push_back(parseLine(line));
            isContinuous.push_back(attrValues.back()[0] == "continuous");
        } else {
            fprintf(stderr, "Unsupported .names file, check line %d\n", lineno);
            fin.close();
            exit(1);
        }
    }
    fin.close();
}

void getData(const std::string &filestub, Rows &data) {
    makeDatastructure(filestub+".names");
    std::ifstream fin(filestub+".data");
    std::string line;
    mins.resize(attrValues.size(), NAN);
    maxs.resize(attrValues.size(), NAN);
    for (int lineno = 0; getline(fin, line); ++lineno) {
        auto values = parseLine(line);
        data.emplace_back(values.size());
        for (int i = 0; i < (int)values.size()-1; ++i) {
            if (values[i] == "?") {
                data.pop_back();
                break;
            }
            if (isContinuous[i]) {
                data.back()[i].f = std::atof(values[i].c_str());
                if (std::isnan(mins[i])) {
                    mins[i] = data.back()[i].f;
                    maxs[i] = data.back()[i].f;
                }
                mins[i] = std::min(mins[i], data.back()[i].f);
                maxs[i] = std::max(maxs[i], data.back()[i].f);
            } else {
                auto x = std::find(attrValues[i].begin(),
                                   attrValues[i].end(),
                                   values[i]);
                if (x == attrValues[i].end()) {
                    fprintf(stderr, "Looking for \"%s\" in:\n", values[i].c_str());
                    for (const auto &v: attrValues[i]) {
                        fprintf(stderr, "%s, ", v.c_str());
                    }
                    fprintf(stderr, "\nCheck line %d\nThat don't exist bro.\n", lineno);
                    exit(1);
                }
                data.back()[i].i = x-attrValues[i].begin();
            }
        }
        auto x = std::find(classes.begin(),
                           classes.end(),
                           values.back());
        if (x == classes.end()) {
            fprintf(stderr, "That class don't exist bro.");
            exit(1);
        }
        data.back().back().i = x-classes.begin();
    }
    fin.close();

    uniqueValueCount.resize(data[0].size()-1);
    for (int column = 0; column < (int)data[0].size()-1; ++column) {
        uniqueValueCount[column] = feature_values(column, data).size();
    }

    if (classes.size() == 2) { // TODO: bilo bi lipo da radi i za vise od 2 klasice
        totalHist.resize(2);
        totalHist[0] = Utils::count(data, 0);
        totalHist[1] = data.size()-totalHist[0];
    }
    minorityClass = min_element(totalHist.begin(), totalHist.end())-totalHist.begin();
}

float accuracy(int TP, int TN, int FP, int FN) {
    return 1.*(TP+TN)/(TP+TN+FP+FN);
}

float BA(int TP, int TN, int FP, int FN) {
    return (1.f*TP/(TP+FN) + 1.f*TN/(FP+TN))/2.f;
}

float fMeasure(int TP, int , int FP, int FN) {
    float precision = 1.*TP/(TP+FP);
    float sensitivity = 1.*TP/(TP+FN);
    return 2.*precision*sensitivity/(precision+sensitivity);
}

float MCC(int TP, int TN, int FP, int FN) {
    return (TP*TN-FP*FN)/sqrt((TP+FP)*(TP+FN)*(TN+FP)*(TN+FN));
}

float AUC(int cl, const std::vector<std::vector<float>> &probs) {
    std::vector<float> pos, neg;
    for (const auto &prob: probs) {
        if ((int)prob.back() == cl) {
            pos.push_back(prob[cl]);
        } else {
            neg.push_back(prob[cl]);
        }
    }
    float critPair = 0;
    for (float p: pos) {
        for (float n: neg) {
            if (Utils::eq(p, n)) {
                critPair += 0.5;
            } else if (p > n) {
                critPair += 1.0;
            }
        }
    }
    return critPair / (pos.size() * neg.size());
}

std::vector<float> class_weights(const std::vector<std::vector<float>> &probs) {
    std::vector<float> weights(classes.size());
    for (const auto &prob: probs) {
        ++weights[(int)prob.back()];
    }
    for (int cl = 0; cl < (int)classes.size(); ++cl) {
        weights[cl] /= probs.size();
    }
    return weights;
}

std::vector<float> calcStats(const std::vector<int> &TP, const std::vector<int> &TN, 
               const std::vector<int> &FP, const std::vector<int> &FN,
               const std::vector<std::vector<float>> &probs) {
    std::vector<float (*)(int, int, int, int)> measures = {
        accuracy, BA, fMeasure, MCC
    };
    fprintf(stderr, "\t\tACC\t\tBA\t\tF-1\t\tMCC\t\tAUC\n");
    const auto weights = class_weights(probs);
    std::vector<float> avgs(5);
    for (int cl = 0; cl < (int)classes.size(); ++cl) {
        fprintf(stderr, "%8s:\t", classes[cl].c_str());
        for (int m = 0; m < (int)measures.size(); ++m) {
            float cur = measures[m](TP[cl], TN[cl], FP[cl], FN[cl]);
            fprintf(stderr, "%8.6f\t", cur);
            avgs[m] += cur;
        }
        float cur = AUC(cl, probs);
        avgs[measures.size()] += cur*weights[cl];
        fprintf(stderr, "%8.6f\n", cur);
    }
    fprintf(stderr, " Average:\t");
    for (int m = 0; m < (int)measures.size(); ++m) {
        avgs[m] /= classes.size();
        fprintf(stderr, "%8.6f\t", avgs[m]);
    }
    fprintf(stderr, "%8.6f\n", avgs[measures.size()]);
    return avgs;
}

Node *train(Rows &data, int max_depth, int max_buckets, bool C45, bool hellinger, bool IGR) {
    Node *tree;
    if (hellinger) {
        if (C45) {
            fprintf(stderr, "Building C4.5 (HD) tree...\n");
            tree = build_C45_tree<find_best_split_C45_hellinger>(data, max_depth, max_buckets);
        } else {
            fprintf(stderr, "Building CART (HD) tree...\n");
            tree = build_CART_tree<find_best_split_hellinger>(data, max_depth, max_buckets);
        }
        //tree = build_hellinger_tree(data, max_depth, max_buckets);
    } else {
        if (C45) {
            if (IGR) {
                fprintf(stderr, "Building C4.5 (IGR) decision tree...\n");
                tree = build_C45_tree<find_best_split_C45_IGR>(data, max_depth, max_buckets);
            } else {
                fprintf(stderr, "Building C4.5 (IG) decision tree...\n");
                tree = build_C45_tree<find_best_split_C45_IG>(data, max_depth, max_buckets);
            }
        } else {
            fprintf(stderr, "Building CART (Gini) decision tree...\n");
            tree = build_CART_tree<find_best_split_indian>(data, max_depth, max_buckets);
            //tree = build_decision_tree(data, max_depth, max_buckets);
        }
    }
    return tree;
}

std::vector<float> test(const Rows &data, Node *tree, bool C45) {
    //print_tree(tree);
    std::vector<int> TP(classes.size()), TN(classes.size()), FP(classes.size()), FN(classes.size());
    std::vector<std::vector<float>> probs;
    for (const auto &row: data) {
        int actual = row.back().i;
        probs.emplace_back(classify(row, tree, C45));
        //printf("Actual: %s. Predicted: ", classes[row.back().i].c_str()); print_leaf(hist, total);
        int prediction = std::max_element(probs.back().begin(), probs.back().end())-probs.back().begin();
        probs.back().push_back(row.back().i);
        for (int cl = 0; cl < (int)classes.size(); ++cl) {
            if (prediction == actual) {
                if (prediction == cl) ++TP[cl];
                else ++TN[cl];
            } else {
                if (prediction == cl) ++FP[cl];
                else ++FN[cl];
            }
        }
    }
    return calcStats(TP, TN, FP, FN, probs);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        fprintf(stderr, "-h\tTo display this help message\n");
        fprintf(stderr, "-s <num>\tTo set the seed, 69 by default\n");
        fprintf(stderr, "-C45\tUse the C4.5 algorithm for building the decision tree. Will use CART if not specified\n");
        fprintf(stderr, "-IGR\tIf not using HD, use Information Gain Ratio instead of Information Gain.\n");
        fprintf(stderr, "-HD\tUse Hellinger distance\n");
        fprintf(stderr, "-f <filestem>\tSet filestem, expects <filestem>.data and <filestem>.names\n");
        // fprintf(stderr, "-t\tUse separate test file, expected <filestem>.test\n"); later
        fprintf(stderr, "-T <num>\tSet train to test ratio if no separate test file, 0.7 by default\n");
        fprintf(stderr, "-c <num1> <num2>\tCrosstrain <num1> times on <num2> folds\n");
        fprintf(stderr, "-b <num>\tSet max buckets for continuous values, 0 means don't use buckets. 0 by default\n");
        fprintf(stderr, "-d <num>\tSet max depth, 999 by default\n");
        fprintf(stderr, "-S\tShuffle dataset, off by default\n");
        return 0;
    }
    int seed = 69;
    bool hellinger = false;
    bool IGR = false;
    bool C45 = false;
    bool shuffle = false;
    bool cv = false;
    std::string filestem; // ew std::string
    float train_to_test_ratio = 0.7f;
    int max_buckets = 0;
    int max_depth = 999;
    int numTimes = -1, numFolds = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0) {
            if (++i < argc) {
                seed = std::atoi(argv[i]);
            } else {
                fprintf(stderr, "-s requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-HD") == 0) {
            hellinger = true;
        } else if (strcmp(argv[i], "-S") == 0) {
            shuffle = true;
        } else if (strcmp(argv[i], "-f") == 0) {
            if (++i < argc) {
                filestem = argv[i];
            } else {
                fprintf(stderr, "-f requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-T") == 0) {
            if (++i < argc) {
                train_to_test_ratio = std::atof(argv[i]);
            } else {
                fprintf(stderr, "-T requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-b") == 0) {
            if (++i < argc) {
                max_buckets = std::atoi(argv[i]);
            } else {
                fprintf(stderr, "-b requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0) {
            if (++i < argc) {
                max_depth = std::atoi(argv[i]);
            } else {
                fprintf(stderr, "-d requires a value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-C45") == 0) {
            C45 = true;
        } else if (strcmp(argv[i], "-IGR") == 0) {
            IGR = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            cv = true;
            if (++i < argc) {
                numTimes = std::atoi(argv[i]);
            } else {
                fprintf(stderr, "-c two values\n");
                return 1;
            }
            if (++i < argc) {
                numFolds = std::atoi(argv[i]);
            } else {
                fprintf(stderr, "-c two values\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unrecognized commandline argument: %s\n", argv[i]);
            return 1;
        }
    }

    BEGIN_SESSION("HDTV"); 
    std::srand(seed);
    Rows data;
    getData(filestem, data);

    if (cv) {
        std::vector<float> avg(5);
        for (int run = 0; run < numTimes; ++run) {
            std::random_shuffle(data.begin(), data.end());
            int passed = 0;
            for (int fold = 0; fold < numFolds; ++fold) {
                printf("Run: %d; Fold: %d\n", run, fold);
                int size = data.size()/numFolds;
                if (fold == numFolds-1) size += data.size()%numFolds;
                Rows training_data(data.size()-size), testing_data(size);
                for (int i = 0, tr = 0, te = 0; i < (int)data.size(); ++i) {
                    if (i < passed) {
                        training_data[tr++] = data[i];
                    } else if (i < passed+size) {
                        testing_data[te++] = data[i];
                    } else {
                        training_data[tr++] = data[i];
                    }
                }
                passed += size;

                Node *tree = train(training_data, max_depth, max_buckets, C45, hellinger, IGR);

                const auto avgs = test(testing_data, tree, C45);
                for (int k = 0; k < (int)avgs.size(); ++k) {
                    avg[k] += avgs[k];
                }
            }
        }
        fprintf(stderr, "\nTot.AVG:\t");
        for (float e: avg) {
            fprintf(stderr, "%8.6f\t", e/(numFolds*numTimes));
        }
    } else {
        if (shuffle) std::random_shuffle(data.begin(), data.end());
        int trainCount = data.size()*train_to_test_ratio;
        Rows training_data(trainCount), testing_data(data.size()-trainCount);
        std::copy_n(data.begin(), trainCount, training_data.begin());
        std::copy_n(data.rbegin(), data.size()-trainCount, testing_data.begin());

        Node *tree = train(training_data, max_depth, max_buckets, C45, hellinger, IGR);

        const auto avgs = test(testing_data, tree, C45);
    }
    printf("\n");
    END_SESSION();
    return 0;
}