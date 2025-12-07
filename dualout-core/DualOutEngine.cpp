#define NOMINMAX  
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "DualOutEngine.h"
#include <mutex>
#include <atomic>
#include <thread>
#include <iostream>
#include <cstring>
#include <chrono>
#include <string>
#include <windows.h>
#include <cmath>
#include <algorithm>
// маленький helper для конвертации std::wstring -> UTF-8
static std::string utf8_from_wide(const std::wstring& ws){
    if(ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if(n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), n, nullptr, nullptr);
    return s;
}

#define MA_CHECK(expr) \
    do { \
        if(!(expr)) { \
            std::cerr << "[DualOutEngine] fail: " #expr << "\n"; \
            ma_device_uninit(&g.devA); \
            ma_device_uninit(&g.devB); \
            ma_pcm_rb_uninit(&g.rbA); \
            ma_pcm_rb_uninit(&g.rbB); \
            ma_context_uninit(&g.ctx); \
            return false; \
        } \
    } while(0)

struct DualOutEngineImpl {

    ma_context ctx{};
    ma_device devA{}, devB{};
    ma_pcm_rb rbA{}, rbB{};
    ma_device_config cfgA{}, cfgB{};
    ma_device_id idA{}, idB{};
    std::atomic_bool running{false};
    uint32_t sr=48000, ch=2;
    ma_uint32 rbCapacityFrames=0;
    std::chrono::steady_clock::time_point lastStats{};
    uint64_t framesSubmitted{0};
    uint64_t dropA{0};
    uint64_t dropB{0};
    std::atomic_bool loggedCallbackA{false};
    std::atomic_bool loggedCallbackB{false};
    std::atomic_bool swapLR{false};

    // НОВОЕ: коэффициенты громкости
    float gainA      = 1.0f;
    float gainB      = 1.0f;
    float masterGain = 1.0f;

    // НОВОЕ: последние измеренные уровни (0..1)
    std::atomic<float> lastRmsL{0.0f};
    std::atomic<float> lastRmsR{0.0f};
    std::atomic<float> lastPeakL{0.0f};
    std::atomic<float> lastPeakR{0.0f};
} g;



static void dev_callback(ma_device* d, void* out, const void*, ma_uint32 frameCount)
{
    const bool isA = (d == &g.devA);
    ma_pcm_rb* rb = (isA ? &g.rbA : &g.rbB);
    const ma_device_config& cfg = isA ? g.cfgA : g.cfgB;
    std::atomic_bool& logged = isA ? g.loggedCallbackA : g.loggedCallbackB;

    if (!logged.exchange(true)) {
        std::cerr << "[DualOut] " << (isA ? "devA" : "devB")
                  << " callback frameCount=" << frameCount
                  << " configuredPeriod=" << cfg.periodSizeInFrames
                  << " periods=" << cfg.periods
                  << " sr=" << g.sr << " ch=" << g.ch << std::endl;
    }

    const ma_uint32 needFrames = frameCount;                 // FRAMES
    const ma_uint32 bpf        = g.ch * sizeof(int16_t);     // bytes per frame

    ma_uint32 totalRead = 0;
    uint8_t* outBytes = static_cast<uint8_t*>(out);

    while (totalRead < needFrames) {
        ma_uint32 remaining = needFrames - totalRead;

        void* p = nullptr;
        ma_uint32 capFrames = remaining;
        if (ma_pcm_rb_acquire_read(rb, &capFrames, &p) != MA_SUCCESS || capFrames == 0) {
            // Нечего читать — выходим из цикла
            break;
        }

        // Копируем то, что реально доступно
        std::memcpy(outBytes + totalRead * bpf, p, capFrames * bpf);
        ma_pcm_rb_commit_read(rb, capFrames);
        totalRead += capFrames;
    }

        // Если кадров не хватило — добиваем тишиной
    if (totalRead < needFrames) {
        ma_uint32 missing = needFrames - totalRead;
        std::memset(outBytes + totalRead * bpf, 0, missing * bpf);
    }

    // НОВОЕ: применяем громкость (на выходе, отдельно для A и B)
        // НОВОЕ: применяем громкость (на выходе, отдельно для A и B)
    float devGain   = isA ? g.gainA : g.gainB;
    float totalGain = devGain * g.masterGain;

    if (std::fabs(totalGain - 1.0f) > 0.0001f) {
        int16_t* samples = reinterpret_cast<int16_t*>(outBytes);
        const size_t sampleCount = static_cast<size_t>(needFrames) * g.ch;
        for (size_t i = 0; i < sampleCount; ++i) {
            float s = static_cast<float>(samples[i]) * totalGain;
            if (s > 32767.0f)  s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            samples[i] = static_cast<int16_t>(s);
        }
    }

    // === NEW: swap L/R if enabled and stereo ===
    if (g.swapLR.load(std::memory_order_relaxed) && g.ch == 2) {
        int16_t* samples = reinterpret_cast<int16_t*>(outBytes);
        const size_t frames = needFrames;
        for (size_t i = 0; i < frames; ++i) {
            std::swap(samples[i*2 + 0], samples[i*2 + 1]); // L <-> R
        }
    }

    // НОВОЕ: считаем RMS/peak по ФАКТИЧЕСКОМУ выходу (после gain+swap), только на A
    if (isA && g.ch >= 1) {
        int16_t* samples = reinterpret_cast<int16_t*>(outBytes);
        const size_t frames = needFrames;

        double sumSqL = 0.0;
        double sumSqR = 0.0;
        float peakL   = 0.0f;
        float peakR   = 0.0f;

        for (size_t i = 0; i < frames; ++i) {
            int16_t sL = samples[i * g.ch + 0];
            int16_t sR = (g.ch > 1 ? samples[i * g.ch + 1] : sL);

            float fl = (float)sL / 32768.0f;
            float fr = (float)sR / 32768.0f;

            sumSqL += (double)fl * (double)fl;
            sumSqR += (double)fr * (double)fr;

            peakL = std::max(peakL, std::fabs(fl));
            peakR = std::max(peakR, std::fabs(fr));
        }

        const double n = (double)frames;
        float rmsL = n > 0.0 ? (float)std::sqrt(sumSqL / n) : 0.0f;
        float rmsR = n > 0.0 ? (float)std::sqrt(sumSqR / n) : 0.0f;

        g.lastRmsL.store(rmsL, std::memory_order_relaxed);
        g.lastRmsR.store(rmsR, std::memory_order_relaxed);
        g.lastPeakL.store(peakL, std::memory_order_relaxed);
        g.lastPeakR.store(peakR, std::memory_order_relaxed);
    }
}



static bool find_device_id_by_name(const std::wstring& wantedW, ma_context* ctx, ma_device_id* outId, std::string* resolved)
{
    if (wantedW.empty()) return false;

    // приводим наше wstring-имя к UTF-8, как в miniaudio
    const std::string wanted = utf8_from_wide(wantedW);

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS)
        return false;

    for (ma_uint32 i = 0; i < count; ++i) {
        const char* nm = infos[i].name; // UTF-8
        if (!nm) continue;

        std::string nmStr(nm);
        // ищем подстроку, чтобы не завязываться на полное совпадение
        if (nmStr.find(wanted) != std::string::npos) {
            *outId = infos[i].id;
            if (resolved) *resolved = nmStr;
            return true;
        }
    }
    return false;
}


bool DualOutEngine::init(const std::wstring& devAName,
                         const std::wstring& devBName,
                         DualOutFormat fmt,
                         bool)
{
    // ЖЁСТКО: всегда работаем на 48000 Hz
    fmt.sr = 48000;

    g.sr = fmt.sr;
    g.ch = fmt.ch;

    // НОВОЕ: сбрасываем громкость
    g.gainA      = 1.0f;
    g.gainB      = 1.0f;
    g.masterGain = 1.0f;


    if (ma_context_init(nullptr, 0, nullptr, &g.ctx) != MA_SUCCESS) {
        std::cerr << "[DualOutEngine] context init failed\n";
        return false;
    }

    // Базовый конфиг для устройства A
    g.cfgA = ma_device_config_init(ma_device_type_playback);
    g.cfgA.playback.format   = ma_format_s16;
    g.cfgA.playback.channels = fmt.ch;
    g.cfgA.sampleRate        = g.sr;
    g.cfgA.dataCallback      = dev_callback;
    g.cfgA.pUserData         = &g.devA;
    g.cfgA.performanceProfile   = ma_performance_profile_low_latency;
    g.cfgA.periods              = 2;
    g.cfgA.periodSizeInFrames   = 480;

    // WASAPI в shared-режиме с авто SRC
    g.cfgA.wasapi.noAutoConvertSRC     = MA_FALSE;
    g.cfgA.wasapi.noDefaultQualitySRC  = MA_FALSE;
    g.cfgA.wasapi.noHardwareOffloading = MA_TRUE;

    // B копирует A
    g.cfgB = g.cfgA;
    g.cfgB.pUserData = &g.devB;

    // --- Поиск девайсов по имени ---
    bool aFound = false;
    bool bFound = false;
    std::string aResolved, bResolved;

    if (!devAName.empty()) {
        aFound = find_device_id_by_name(devAName, &g.ctx, &g.idA, &aResolved);
    }
    if (!devBName.empty()) {
        bFound = find_device_id_by_name(devBName, &g.ctx, &g.idB, &bResolved);
    }

    if (aFound) {
        g.cfgA.playback.pDeviceID = &g.idA;
    } else {
        std::cerr << "[DualOutEngine] A not found, using default\n";
    }

    if (bFound) {
        g.cfgB.playback.pDeviceID = &g.idB;
    } else {
        std::cerr << "[DualOutEngine] B not found, using default\n";
    }

    // --- Инициализация устройств ---
    if (ma_device_init(&g.ctx, &g.cfgA, &g.devA) != MA_SUCCESS ||
        ma_device_init(&g.ctx, &g.cfgB, &g.devB) != MA_SUCCESS)
    {
        std::cerr << "[DualOutEngine] device init failed\n";
        ma_context_uninit(&g.ctx);
        return false;
    }

// --- Ринг-буферы: 2 секунды на 48000 Hz ---
ma_uint32 capacityFrames = g.sr * 2;

if (ma_pcm_rb_init(ma_format_s16, fmt.ch, capacityFrames, nullptr, nullptr, &g.rbA) != MA_SUCCESS ||
    ma_pcm_rb_init(ma_format_s16, fmt.ch, capacityFrames, nullptr, nullptr, &g.rbB) != MA_SUCCESS)
{
    std::cerr << "[DualOutEngine] rb init failed\n";
    ma_device_uninit(&g.devA);
    ma_device_uninit(&g.devB);
    ma_context_uninit(&g.ctx);
    return false;
}

g.rbCapacityFrames = capacityFrames;

    g.framesSubmitted   = 0;
    g.dropA             = 0;
    g.dropB             = 0;
    g.lastStats         = std::chrono::steady_clock::time_point{};
    g.loggedCallbackA.store(false);
    g.loggedCallbackB.store(false);

    // НОВОЕ: сбрасываем уровни
    g.lastRmsL.store(0.0f, std::memory_order_relaxed);
    g.lastRmsR.store(0.0f, std::memory_order_relaxed);
    g.lastPeakL.store(0.0f, std::memory_order_relaxed);
    g.lastPeakR.store(0.0f, std::memory_order_relaxed);

    // --- Праймим буферы нулями ---
   {
    const ma_uint32 prime = g.rbCapacityFrames;
    const ma_uint32 bpf   = g.ch * sizeof(int16_t);

    // A
    void* pA = nullptr; ma_uint32 capA = 0;
    if (ma_pcm_rb_acquire_write(&g.rbA, &capA, &pA) == MA_SUCCESS) {
        ma_uint32 n = ma_min(capA, prime);
        if (pA && n) std::memset(pA, 0, n * bpf);
        ma_pcm_rb_commit_write(&g.rbA, n);
    }

    // B
    void* pB = nullptr; ma_uint32 capB = 0;
    if (ma_pcm_rb_acquire_write(&g.rbB, &capB, &pB) == MA_SUCCESS) {
        ma_uint32 n = ma_min(capB, prime);
        if (pB && n) std::memset(pB, 0, n * bpf);
        ma_pcm_rb_commit_write(&g.rbB, n);
    }
}


    // --- Стартуем устройства ---
    MA_CHECK(ma_device_start(&g.devA) == MA_SUCCESS);
    MA_CHECK(ma_device_start(&g.devB) == MA_SUCCESS);

    // СТАЛО
    g.running = true;
    std::cerr << "[DualOutEngine] started A=[" << (aFound ? aResolved : "default")
            << "] B=[" << (bFound ? bResolved : "default")
            << "] @" << fmt.sr << "Hz ch=" << fmt.ch << "\n";

    return true;

}

bool DualOutEngine::write(const void* data, size_t frames, int64_t)
{
    if (!g.running) return false;

    const ma_uint32 inFrames = (ma_uint32)frames;        // входные кадры
    const ma_uint32 bpf      = g.ch * sizeof(int16_t);   // bytes per frame
    const uint8_t* srcBytes  = static_cast<const uint8_t*>(data);

    // --- Пишем в A ---
    ma_uint32 remainingA = inFrames;
    ma_uint32 wroteA = 0;
    while (remainingA > 0) {
        void* p = nullptr;
        ma_uint32 capFrames = remainingA;
        if (ma_pcm_rb_acquire_write(&g.rbA, &capFrames, &p) != MA_SUCCESS || capFrames == 0) {
            break;  // буфер переполнен
        }
        std::memcpy(p, srcBytes + wroteA * bpf, capFrames * bpf);
        ma_pcm_rb_commit_write(&g.rbA, capFrames);
        wroteA     += capFrames;
        remainingA -= capFrames;
    }

    // --- Пишем в B ---
    ma_uint32 remainingB = inFrames;
    ma_uint32 wroteB = 0;
    while (remainingB > 0) {
        void* p = nullptr;
        ma_uint32 capFrames = remainingB;
        if (ma_pcm_rb_acquire_write(&g.rbB, &capFrames, &p) != MA_SUCCESS || capFrames == 0) {
            break;  // буфер переполнен
        }
        std::memcpy(p, srcBytes + wroteB * bpf, capFrames * bpf);
        ma_pcm_rb_commit_write(&g.rbB, capFrames);
        wroteB     += capFrames;
        remainingB -= capFrames;
    }

    g.framesSubmitted += inFrames;
    if (wroteA < inFrames) g.dropA += (inFrames - wroteA);
    if (wroteB < inFrames) g.dropB += (inFrames - wroteB);

    auto now = std::chrono::steady_clock::now();
    if (g.lastStats == std::chrono::steady_clock::time_point{}) {
        g.lastStats = now;
    }

    if (now - g.lastStats >= std::chrono::seconds(1)) {
        const ma_uint32 availReadA  = ma_pcm_rb_available_read(&g.rbA);
        const ma_uint32 availWriteA = ma_pcm_rb_available_write(&g.rbA);
        const ma_uint32 availReadB  = ma_pcm_rb_available_read(&g.rbB);
        const ma_uint32 availWriteB = ma_pcm_rb_available_write(&g.rbB);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g.lastStats).count();

        int qAms  = queueMsA();
        int qBms  = queueMsB();
        int drift = driftMsAB();

        std::cerr << "[DualOut] rbA read=" << availReadA  << "/" << g.rbCapacityFrames
                  << " write=" << availWriteA << "/" << g.rbCapacityFrames
                  << " | rbB read=" << availReadB  << "/" << g.rbCapacityFrames
                  << " write=" << availWriteB << "/" << g.rbCapacityFrames
                  << " feedFrames=" << g.framesSubmitted
                  << " dropA=" << g.dropA
                  << " dropB=" << g.dropB
                  << " windowMs=" << elapsedMs
                  << " queueA_ms=" << qAms
                  << " queueB_ms=" << qBms
                  << " drift_ms=" << drift
                  << std::endl;

        g.lastStats       = now;
        g.framesSubmitted = 0;
        g.dropA           = 0;
        g.dropB           = 0;
    }

    return true;
}


void DualOutEngine::drain(){ std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

// СТАЛО
void DualOutEngine::stop()
{
    if(g.running.exchange(false)){
        ma_device_uninit(&g.devA);
        ma_device_uninit(&g.devB);
        ma_pcm_rb_uninit(&g.rbA);
        ma_pcm_rb_uninit(&g.rbB);
        ma_context_uninit(&g.ctx);
        std::cerr << "[DualOutEngine] stopped\n";
    }
}


// НОВОЕ: установка громкости в dB + master 0..1
void DualOutEngine::setGainDb(float aDb, float bDb, float masterLinear) {
    auto dbToLin = [](float db) -> float {
        return std::pow(10.0f, db / 20.0f);
    };

    g.gainA = dbToLin(aDb);
    g.gainB = dbToLin(bDb);
    g.masterGain = std::clamp(masterLinear, 0.0f, 1.0f);
}

// НОВОЕ: пока задержку не используем — заглушка
void DualOutEngine::setDelayMs(int, int) {
    // можно реализовать позже, сейчас просто чтобы был body
}

// НОВОЕ: очистка очередей (для seek)
void DualOutEngine::flush() {
    if (!g.running.load()) return;
    ma_pcm_rb_reset(&g.rbA);
    ma_pcm_rb_reset(&g.rbB);
}
void DualOutEngine::setSwapLR(bool v) {
    g.swapLR.store(v, std::memory_order_relaxed);
}

int DualOutEngine::queueMsA() const {
    if (!g.running.load() || g.sr == 0) return 0;
    ma_uint32 frames = ma_pcm_rb_available_read(const_cast<ma_pcm_rb*>(&g.rbA));
    double ms = (double)frames * 1000.0 / (double)g.sr;
    return (int)ms;
}

int DualOutEngine::queueMsB() const {
    if (!g.running.load() || g.sr == 0) return 0;
    ma_uint32 frames = ma_pcm_rb_available_read(const_cast<ma_pcm_rb*>(&g.rbB));
    double ms = (double)frames * 1000.0 / (double)g.sr;
    return (int)ms;
}

int DualOutEngine::driftMsAB() const {
    // Положительное значение = в A больше очереди, чем в B
    int qa = queueMsA();
    int qb = queueMsB();
    return qa - qb;
}

// НОВОЕ: отдать последние уровни (0..1), если движок запущен
bool DualOutEngine::getLevels(float& rmsL, float& rmsR, float& peakL, float& peakR) const {
    if (!g.running.load(std::memory_order_relaxed)) {
        rmsL = rmsR = peakL = peakR = 0.0f;
        return false;
    }
    rmsL  = g.lastRmsL.load(std::memory_order_relaxed);
    rmsR  = g.lastRmsR.load(std::memory_order_relaxed);
    peakL = g.lastPeakL.load(std::memory_order_relaxed);
    peakR = g.lastPeakR.load(std::memory_order_relaxed);
    return true;
}


