//
//  train_embed_algo.cpp
//  LightCTR
//
//  Created by SongKuangshi on 2017/10/17.
//  Copyright © 2017年 SongKuangshi. All rights reserved.
//

#include "train_embed_algo.h"
#include <string.h>
#include <stack>
#include "../util/product_quantizer.h"

#define FOR_D for (int i = 0; i < emb_dimention; i++)

void Train_Embed_Algo::init() {
    learning_rate = 0.02f;
    negSample_cnt = 12;
    
    treeArry = new Node[vocab_cnt * 2];
    size_t treeNode_cnt = 0;
    // build Huffman
    priority_queue<pair<int, int>, vector<pair<int, int> >, cmp> Q;
    Node *curNode;
    int index = (int)vocab_cnt - 1;
    for (; index >= 0; index--) {
        curNode = treeArry + treeNode_cnt;
        curNode->frequency = word_frequency[index];
        Q.push(make_pair(word_frequency[index], treeNode_cnt));
        treeNode_cnt++;
    }
    while (Q.size() > 1) {
        pair<int, int> p1 = Q.top();
        Q.pop();
        pair<int, int> p2 = Q.top();
        Q.pop();
        curNode = treeArry + treeNode_cnt;
        curNode->weight = new double[emb_dimention];
        // hierarchical softmax weight init with 0
        memset(curNode->weight, 0, sizeof(double) * emb_dimention);
        
        curNode->frequency = p1.first + p2.first;
        curNode->left = p1.second;
        curNode->right = p2.second;
        Q.push(make_pair(p1.first + p2.first, treeNode_cnt));
        treeNode_cnt++;
    }
    treeRoot = treeArry + Q.top().second;
    Q.pop();
    assert(vocab_cnt * 2 - 1 == treeNode_cnt);
    
    // generate Huffman code, less frequency first
    word_code.resize(vocab_cnt);
    stack<pair<int, string> > S;
    S.push(make_pair(treeRoot - treeArry, string()));
    while (!S.empty()) {
        pair<size_t, string> p = S.top();
        pair<size_t, string> p2 = S.top();
        S.pop();
        curNode = treeArry + p.first;
        if (curNode->left == -1) { // get leaf
            assert(p.first < vocab_cnt);
            word_code[vocab_cnt - 1 - p.first] = p.second;
            continue;
        }
        p.first = curNode->left;
        p.second.append(move("1"));
        S.push(p);
        p2.first = curNode->right;
        p2.second.append(move("0"));
        S.push(p2);
    }
}

void Train_Embed_Algo::Train() {
    threadpool->init();
    size_t docid = 0;
    string line;
    while(!textStream.eof()){
        getline(textStream, line);
        size_t offset = textStream.tellg();
        if (line == "<TEXT>") {
            threadpool->addTask(bind(&Train_Embed_Algo::TrainDocument, this, docid++, offset));
        }
    }
    threadpool->join();
    cout << "All " << docid << " docs are trained completely" << endl;
    
    // Normalization
    for (size_t wid = 0; wid < vocab_cnt; wid++) {
        auto norm = avx_L2Norm(&word_embedding[wid * emb_dimention], emb_dimention);
        FOR_D {
            word_embedding[wid * emb_dimention + i] /= norm;
        }
    }
    
    saveModel();
}

void Train_Embed_Algo::TrainDocument(size_t docid, size_t offset) {
    ifstream textStream_thread;
    loadTextFile(&textStream_thread);
    textStream_thread.seekg(offset);
    
    char word[128];
    vector<size_t> doc_wordid_vec;
    doc_wordid_vec.reserve(256);
    // extract all words id in one document
    while (NextWord(&textStream_thread, word) > 0) {
        auto it = vocabTable.find(string(word));
        if (it == vocabTable.end()) {
            continue;
        }
        if (subsampling > 0) { // subsample high frequent words
            float freq = word_frequency[vocabTable[word]];
            float prob = (sqrt(freq / (subsampling * total_words_cnt)) + 1) *
                        (subsampling * total_words_cnt) / freq;
            
            if (UniformNumRand() > prob)
                continue;
        }
        doc_wordid_vec.emplace_back(it->second);
    }
    if(doc_wordid_vec.size() <= window_size * 2 + 1) {
        return;
    }
    // N-Gram CBOW continuous bag of word
    // TODO skip-gram impl
    // cbow hsoftmax and negative discriminant
    vector<double> ctx_sum, emb_delta;
    ctx_sum.resize(emb_dimention);
    emb_delta.resize(emb_dimention);
    double decay_alpha = learning_rate;
    
    // each document trains epoch times
    for (size_t ep = 0; ep < epoch; ep++) {
        decay_alpha *= 0.96f;
        decay_alpha = max(decay_alpha, 0.0001);
        
        int length = (int)doc_wordid_vec.size();
        for (int word_index = 0; word_index < length; word_index++) {
            FOR_D {
                ctx_sum[i] = 0;
                emb_delta[i] = 0;
            }
            // sum of embedding vector in window
            const int first = max(0, word_index - (int)window_size);
            const int last = min(length, word_index + (int)window_size);
            for (int pos = first; pos < last; pos++) {
                if (pos == word_index) {
                    continue;
                }
                size_t wid = doc_wordid_vec[pos];
                FOR_D {
                    ctx_sum[i] += word_embedding[wid * emb_dimention + i];
                }
            }
            
            // train hierarchical softmax
            const size_t cur_wid = doc_wordid_vec[word_index];
            const Node* curNode = treeRoot;
            for (size_t c = 0; c < word_code[cur_wid].length(); c++) {
                int realdir = word_code[cur_wid][c] - '0';
                double preddir = 0.0f;
                FOR_D {
                    preddir += curNode->weight[i] * ctx_sum[i];
                }
                if(!(preddir > -30 && preddir < 30)) {
                    printf("-- warning hiso %zu-%zu preddir = %lf\n", cur_wid, c, preddir);
                }
                preddir = sigmoid.forward(preddir);
                // LR gradient to max Loglikelihood
                double gradient = decay_alpha * (realdir - preddir);
                FOR_D {
                    emb_delta[i] += gradient * curNode->weight[i];
                    curNode->weight[i] += gradient * ctx_sum[i];
                }
            }
            
            // train negative discriminant
            size_t label, wid = 0;
            for (size_t itr = 0; itr < negSample_cnt + 1; itr++) {
                if (itr == 0) {
                    label = 1;
                    wid = cur_wid;
                } else {
                    label = 0;
                    do { // smaple negative word
                        wid = negSampleTable[rand() % negTable_size];
                    } while (wid == cur_wid);
                }
                double preddir = 0.0f;
                FOR_D {
                    preddir += negWeight[wid][i] * ctx_sum[i];
                }
                if(!(preddir > -30 && preddir < 30)) {
                    printf("-- warning negsa %zu preddir = %lf\n", cur_wid, preddir);
                }
                preddir = sigmoid.forward(preddir);
                // LR gradient to max Loglikelihood
                double gradient = decay_alpha * (label - preddir);
                FOR_D {
                    emb_delta[i] += gradient * negWeight[wid][i];
                    negWeight[wid][i] += gradient * ctx_sum[i];
                }
            }
            
            // unsafe multi-thread update word embedding
            for (int pos = first; pos < last; pos++) {
                size_t wid = doc_wordid_vec[pos];
                FOR_D {
                    word_embedding[wid * emb_dimention + i] += emb_delta[i];
                }
            }
        }
    }
    cout << "Train docid " << docid << " has " << doc_wordid_vec.size()
                      << " words" << " alpha = " << decay_alpha << endl;
}

void Train_Embed_Algo::Quantization(size_t part_cnt, uint8_t cluster_cnt) {
    Product_quantizer<float, uint8_t> *pq =
        new Product_quantizer<float, uint8_t>(emb_dimention, part_cnt, cluster_cnt);
    auto quantizated_codes = pq->train(word_embedding.data(), vocab_cnt);
    
    ofstream md("./output/quantized_embedding.txt");
    if(!md.is_open()){
        cout<<"save model open file error" << endl;
        exit(1);
    }
    for (size_t wid = 0; wid < vocab_cnt; wid++) {
        for (int i = 0; i < part_cnt; i++) {
            md << static_cast<int>(quantizated_codes[i][wid]) << " ";
        }
        md << endl;
    }
    md << endl;
    md.close();
}

void Train_Embed_Algo::EmbeddingCluster(shared_ptr<vector<int> > clustered, size_t cluster_cnt) {
    
    vector<vector<string>* > topicSet;
    for (size_t c = 0; c < cluster_cnt; c++) {
        topicSet.emplace_back(new vector<string>);
        topicSet[c]->reserve(vocab_cnt);
    }
    for (size_t wid = 0; wid < clustered->size(); wid++) {
        topicSet[clustered->at(wid)]->emplace_back(vocabString[wid]);
    }
    ofstream md("./output/word_cluster.txt");
    if(!md.is_open()){
        cout<<"save model open file error" << endl;
        exit(1);
    }
    for (size_t c = 0; c < cluster_cnt; c++) {
        md << "Cluster " << c << ":";
        for (auto it = topicSet[c]->begin(); it != topicSet[c]->end(); it++) {
            md << " " << *it;
        }
        md << endl;
    }
    md.close();
}

void Train_Embed_Algo::saveModel() {
    ofstream md("./output/word_embedding.txt");
    if(!md.is_open()){
        cout<<"save model open file error" << endl;
        exit(1);
    }
    for (size_t wid = 0; wid < vocab_cnt; wid++) {
        FOR_D {
            md << word_embedding[wid * emb_dimention + i] << " ";
        }
        md << endl;
    }
    md << endl;
    md.close();
}
