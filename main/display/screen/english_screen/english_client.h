#pragma once

#include <string>

#include "english_types.h"

class EnglishClient {
public:
    static EnglishTaskList FetchTasks(std::string& error);
    static EnglishReview FetchReview(const std::string& task_id,
                                     std::string& error);
    static bool SubmitResult(const std::string& task_id,
                             const std::string& word_id,
                             const std::string& result,
                             std::string& error);
    static EnglishReview LoadCachedReview(const std::string& task_id,
                                          std::string& error);
    // 开机后台预取任务列表，落 SD 缓存，首次进入可先显示。
    static void PrefetchTasksAsync();
};
