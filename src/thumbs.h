#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ThumbRequest {
    size_t generation = 0;
    int index = -1;
    std::wstring path;
};

struct ThumbResult {
    ThumbRequest request;
    UINT width = 0, height = 0;
    std::vector<BYTE> pixels;             // PBGRA；GPU 位图由界面线程创建
};

// 一个后台线程顺序解码；待办始终只保留当前视口及缓冲行。
class ThumbLoader {
public:
    ~ThumbLoader() { stop(); }
    void submit(HWND hwnd, UINT message, size_t generation, std::vector<ThumbRequest> requests);
    std::vector<ThumbResult> takeResults();
    void cancel(size_t generation);
    void stop();

private:
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_ = false;
    HWND hwnd_ = nullptr;
    UINT message_ = 0;
    size_t generation_ = 0;
    ThumbRequest active_;
    std::vector<ThumbRequest> requests_;
    std::vector<ThumbResult> results_;
};
