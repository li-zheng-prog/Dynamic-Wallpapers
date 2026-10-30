#include "thumbs.h"
#include <wincodec.h>
#include <algorithm>

static void DecodeThumb(IWICImagingFactory* wic, ThumbResult& result) {
    if (!wic) return;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    auto release = [&] {
        if (converter) converter->Release();
        if (scaler) scaler->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
    };
    if (FAILED(wic->CreateDecoderFromFilename(result.request.path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
        release();
        return;
    }
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height) {
        release();
        return;
    }
    // 同时约束高和宽，避免异常长图产生过大的解码缓冲。
    double scale = (std::min)(1.0, (std::min)(420.0 / width, 4096.0 / height));
    result.width = (std::max)(1u, (UINT)(width * scale));
    result.height = (std::max)(1u, (UINT)(height * scale));
    IWICBitmapSource* source = frame;
    if (result.width != width || result.height != height) {
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame, result.width, result.height,
                                     WICBitmapInterpolationModeFant))) {
            release();
            return;
        }
        source = scaler;
    }
    if (SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
        UINT stride = result.width * 4;
        result.pixels.resize((size_t)stride * result.height);
        if (FAILED(converter->CopyPixels(nullptr, stride, (UINT)result.pixels.size(),
                                        result.pixels.data())))
            result.pixels.clear();
    }
    release();
}

void ThumbLoader::submit(HWND hwnd, UINT message, size_t generation,
                         std::vector<ThumbRequest> requests) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    hwnd_ = hwnd;
    message_ = message;
    generation_ = generation;
    auto same = [](const ThumbRequest& a, const ThumbRequest& b) {
        return a.generation == b.generation && a.index == b.index && a.path == b.path;
    };
    requests.erase(std::remove_if(requests.begin(), requests.end(), [&](const ThumbRequest& r) {
        if (same(r, active_)) return true;
        for (const auto& ready : results_) if (same(r, ready.request)) return true;
        return false;
    }), requests.end());
    requests_ = std::move(requests);
    if (!requests_.empty() && !worker_.joinable()) worker_ = std::thread(&ThumbLoader::run, this);
    wake_.notify_one();
}

std::vector<ThumbResult> ThumbLoader::takeResults() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ThumbResult> ready;
    ready.swap(results_);
    return ready;
}

void ThumbLoader::cancel(size_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    requests_.clear();
    results_.clear();
}

void ThumbLoader::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        requests_.clear();
        results_.clear();
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void ThumbLoader::run() {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(com)) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_IWICImagingFactory, (void**)&wic);
    for (;;) {
        ThumbResult result;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !requests_.empty(); });
            if (stopping_) break;
            result.request = std::move(requests_.front());
            requests_.erase(requests_.begin());
            active_ = result.request;
        }
        DecodeThumb(wic, result);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = ThumbRequest{};
            if (!stopping_ && result.request.generation == generation_) {
                results_.push_back(std::move(result));
                // 消息只负责唤醒，像素仍由队列持有；销毁窗口不会遗留裸指针。
                PostMessageW(hwnd_, message_, 0, 0);
            }
        }
    }
    if (wic) wic->Release();
    if (SUCCEEDED(com)) CoUninitialize();
}
