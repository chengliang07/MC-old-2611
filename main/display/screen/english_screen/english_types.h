#pragma once

#include <string>
#include <vector>

struct EnglishTask {
    std::string id;
    std::string task_date;
    std::string status;
    int phrase_count = 0;
    bool can_review = false;
};

struct EnglishEntry {
    std::string word_id;
    bool word_id_from_server = false;
    std::string term;
    std::string phonetic;
    std::string meaning;
    std::string phrase;
    std::string example;
    std::vector<std::string> options;
    int correct_index = -1;
};

struct EnglishReview {
    std::string label;
    std::vector<EnglishEntry> entries;
};

using EnglishTaskList = std::vector<EnglishTask>;
