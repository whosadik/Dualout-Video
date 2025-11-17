#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <vector>
#include <chrono>
#include <mfidl.h>
#include <wrl/client.h>
#include "dualout_bridge.h" // чтобы писать PCM в DualOutEngine
#include <windows.h>
#include <evr.h>
struct ComInit; struct MFInit; // forward

class PlayerCore {
public:
    PlayerCore();
    ~PlayerCore();

    // Открывает медиа и готовит поток (автозапуск = false)
    bool open(const std::wstring& url, DualOutBridge* bridge /*не владеем*/);

    // Управление
    bool play();
    bool pause();
    bool stop();
    bool seek_ms(int64_t ms);
  bool set_hwnd(HWND hwnd);
    // Статус в JSON-строке без зависимостей
    std::string status_json() const;
  // diagnostics for error reporting
   long last_hr() const { return lastHr_; }
    const std::string& last_err() const { return lastErr_; }
private:
    void worker_loop();
    bool refresh_reader_media_type(const char* reason);
    void convert_samples_to_s16(const uint8_t* src, size_t frames);
    bool needs_sample_conversion() const;
    void log_feed_stats(size_t frames, bool writeOk);
  bool build_video_session();      // создать сессию EVR по текущему url_ и hwnd_
    void destroy_video_session();    // освободить
    bool video_start();              // старт
    bool video_pause();              // пауза
    bool video_stop();               // стоп
    bool video_seek_100ns(LONGLONG pos);
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    DualOutBridge* bridge_ = nullptr;
 std::unique_ptr<ComInit> com_;
    std::unique_ptr<MFInit>  mf_;
    std::thread th_;
    std::atomic_bool stop_{false};
    std::atomic_bool paused_{true};

    mutable std::mutex mtx_;
    std::condition_variable cv_;

    // Текущий формат входа (после конверсии в PCM 16)
    PcmDesc fmt_{48000,2,16};
    GUID readerSubtype_{};
    uint32_t readerBitsPerSample_{16};
    uint32_t readerBytesPerFrame_{0};
    bool convertFloatToS16_{false};
    bool downmixIntToS16_{false};
    std::vector<int16_t> scratch16_;
    std::chrono::steady_clock::time_point feedLogStart_{};
    uint64_t framesFedSinceLog_{0};
    uint64_t failedWritesSinceLog_{0};

    // Позиция и длительность (100-нс и мс)
    std::atomic<long long> last_pts_100ns_{0};
    long long duration_100ns_{0};
    std::wstring url_;
    std::atomic<bool> opened_{false};\
        // --- видео (EVR + Media Session)
    HWND hwnd_{nullptr};
    Microsoft::WRL::ComPtr<IMFMediaSession> vSession_;
    Microsoft::WRL::ComPtr<IMFMediaSource>  vSource_;
      bool videoReady_{false};
    long lastHr_{0};
    std::string lastErr_;
};
