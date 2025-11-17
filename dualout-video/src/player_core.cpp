#include "player_core.h"
#include "mf_utils.h"
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mftransform.h>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {
std::string hr_to_string(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<unsigned long>(hr);
    return oss.str();
}

std::string guid_to_utf8(const GUID& guid) {
    wchar_t wide[64]{};
    int chars = StringFromGUID2(guid, wide, ARRAYSIZE(wide));
    if (chars <= 0) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), bytes - 1, nullptr, nullptr);
    return result;
}

void log_media_type(const char* prefix, IMFMediaType* type) {
    if (!type) return;
    UINT32 sr = 0, ch = 0, bps = 0, block = 0;
    GUID subtype{};
    type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
    type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &block);
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    std::cerr << "[PlayerCore] " << prefix
              << " sr=" << sr
              << " ch=" << ch
              << " bps=" << bps
              << " block=" << block
              << " subtype=" << guid_to_utf8(subtype)
              << std::endl;
}

void log_codec_hint(HRESULT hr) {
    std::cerr << "[PlayerCore] Media Foundation reader failed (" << hr_to_string(hr)
              << "). If this file carries AAC audio (e.g. MOV/MP4) install the Media "
              << "Feature Pack or register the AAC decoder (CLSID_CMSAACDecMFT)." << std::endl;
}
} // namespace
static bool setReaderToPcm16(ComPtr<IMFSourceReader>& r, PcmDesc& fmt) {
    ComPtr<IMFMediaType> native;
    HRESULT hr = r->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native);
    if (FAILED(hr)) {
        std::cerr << "[PlayerCore] GetNativeMediaType failed hr=" << hr_to_string(hr) << std::endl;
        return false;
    }
    log_media_type("native-audio-type", native.Get());

    // Наш целевой формат: 48000 / 2 / 16
    fmt.sr  = 48000;
    fmt.ch  = 2;
    fmt.bps = 16;

    ComPtr<IMFMediaType> out;
    hr = MFCreateMediaType(&out);
    if (FAILED(hr)) {
        std::cerr << "[PlayerCore] MFCreateMediaType failed hr=" << hr_to_string(hr) << std::endl;
        return false;
    }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, fmt.sr);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       fmt.ch);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    fmt.bps);
    out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,    fmt.ch * (fmt.bps / 8));
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, fmt.sr * fmt.ch * (fmt.bps / 8));

    hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, out.Get());
    if (FAILED(hr)) {
        std::cerr << "[PlayerCore] SetCurrentMediaType(PCM16) failed hr=" << hr_to_string(hr) << std::endl;
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current))) {
            log_media_type("reader-current-after-fail", current.Get());
        }
        return false;
    }

    ComPtr<IMFMediaType> resolved;
    if (SUCCEEDED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &resolved))) {
        log_media_type("resolved-reader-type", resolved.Get());
    }
    return true;
}

PlayerCore::PlayerCore() {
    readerBytesPerFrame_ = fmt_.ch * (fmt_.bps / 8);
}
PlayerCore::~PlayerCore(){ stop(); }

bool PlayerCore::open(const std::wstring& url, DualOutBridge* bridge){
    stop(); // на всякий
    bridge_ = bridge;
    url_ = url;

    com_ = std::make_unique<ComInit>();
    mf_  = std::make_unique<MFInit>();

    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 2))) return false;
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attr->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);

    ComPtr<IMFSourceReader> r;
    HRESULT hr = MFCreateSourceReaderFromURL(url.c_str(), attr.Get(), &r);
    if (FAILED(hr)) {
        std::cerr << "[PlayerCore] MFCreateSourceReaderFromURL failed hr=" << hr_to_string(hr) << std::endl;
        log_codec_hint(hr);
        ComPtr<IMFByteStream> byteStream;
        HRESULT fileHr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, url.c_str(), &byteStream);
        if (FAILED(fileHr)) {
            std::cerr << "[PlayerCore] MFCreateFile fallback failed hr=" << hr_to_string(fileHr) << std::endl;
            return false;
        }
        hr = MFCreateSourceReaderFromByteStream(byteStream.Get(), attr.Get(), &r);
        if (FAILED(hr)) {
            std::cerr << "[PlayerCore] MFCreateSourceReaderFromByteStream failed hr=" << hr_to_string(hr) << std::endl;
            log_codec_hint(hr);
            return false;
        }
        std::cerr << "[PlayerCore] Fallback to byte-stream reader succeeded" << std::endl;
    }

    if (!setReaderToPcm16(r, fmt_)) return false;
    reader_ = r;
    convertFloatToS16_ = false;
    downmixIntToS16_ = false;
    readerBitsPerSample_ = fmt_.bps;
    readerBytesPerFrame_ = fmt_.ch * (fmt_.bps / 8);
    framesFedSinceLog_ = 0;
    failedWritesSinceLog_ = 0;
    feedLogStart_ = {};
    if (!refresh_reader_media_type("initial-reader-type")) return false;

    // длительность
    ComPtr<IMFPresentationDescriptor> pd;
    ComPtr<IMFMediaSource> src;
    {
        // хитрый путь: через атрибуты у ридера не всегда есть длительность, поэтому создаём из URL вторично
        ComPtr<IMFSourceResolver> resolver;
        MFCreateSourceResolver(&resolver);
        MF_OBJECT_TYPE objType;
        IUnknown* unk = nullptr;
        if (SUCCEEDED(resolver->CreateObjectFromURL(url.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr, &objType, &unk))){
            unk->QueryInterface(IID_PPV_ARGS(&src));
            unk->Release();
        }
        if (src){
            src->CreatePresentationDescriptor(&pd);
            PROPVARIANT var{}; PropVariantInit(&var);
            if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, (UINT64*)&duration_100ns_))) {
                // ok
            }
        }
    }
    last_pts_100ns_.store(0);
    stop_.store(false);
    paused_.store(true);
    opened_.store(true);

    // Запускаем рабочий поток, но в паузе
    th_ = std::thread(&PlayerCore::worker_loop, this);
    return true;
}

bool PlayerCore::play(){
    if(!opened_.load()) return false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        paused_.store(false);
    }
    cv_.notify_all();
      if (videoReady_) video_start();
    return true;
}

bool PlayerCore::pause(){
    if(!opened_.load()) return false;
    paused_.store(true);
     if (videoReady_) video_pause();
    return true;
}

bool PlayerCore::stop(){
    bool wasOpen = opened_.exchange(false);
    stop_.store(true);
    cv_.notify_all();
    if (th_.joinable()) th_.join();
     if (videoReady_) { video_stop(); destroy_video_session(); }
    reader_.Reset();
    vSource_.Reset();
    vSession_.Reset();
    videoReady_ = false;
    mf_.reset();
    com_.reset();
    return wasOpen;
}

bool PlayerCore::seek_ms(int64_t ms){
    if (!opened_.load()) return false;

    // Запоминаем, играл ли плеер до seek
    const bool wasPlaying = !paused_.load();

    // Ставим на паузу и стопаем воркер
    paused_.store(true);
    cv_.notify_all();

    // Переводим MF reader на новую позицию (в 100-нс)
    const LONGLONG pts100ns = ms * 10000;
    PROPVARIANT pos{};
    pos.vt = VT_I8;
    pos.hVal.QuadPart = pts100ns;
    HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    if (FAILED(hr)) {
        return false;
    }

    last_pts_100ns_.store(pts100ns);

    // Сбрасываем очереди dualout, чтобы не было хвостов старого аудио
    if (bridge_) {
        bridge_->eng.flush();
    }

    // Видео тоже перескакиваем
    if (videoReady_) {
        video_seek_100ns(pts100ns);
    }

    // Если до seek играло — продолжаем воспроизведение
    if (wasPlaying) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            paused_.store(false);
        }
        cv_.notify_all();
        if (videoReady_) {
            video_start();
        }
    }

    return true;
}

bool PlayerCore::set_hwnd(HWND hwnd){
    hwnd_ = hwnd;
    // если уже открыт url_ — готовим видео-сессию
    if (opened_.load() && hwnd_) {
        destroy_video_session();
           if (!com_)  com_ = std::make_unique<ComInit>();
        if (!mf_)   mf_  = std::make_unique<MFInit>();
        videoReady_ = build_video_session();
        return videoReady_;
    }
    return true;
}

std::string PlayerCore::status_json() const{
    const bool isOpen = opened_.load();
    const bool isPaused = paused_.load();
    long long pts = last_pts_100ns_.load();
    long long dur = duration_100ns_;
    auto ms = pts / 10000;
    auto total = dur>0 ? dur/10000 : 0;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"state\":\"%s\",\"pos_ms\":%lld,\"dur_ms\":%lld,"
        "\"sr\":%u,\"ch\":%u,\"bps\":%u}",
        (!isOpen ? "stopped" : (isPaused ? "paused" : "playing")),
        (long long)ms, (long long)total,
        fmt_.sr, fmt_.ch, fmt_.bps);
    return std::string(buf);
}

bool PlayerCore::refresh_reader_media_type(const char* reason){
    if (!reader_) return false;
    ComPtr<IMFMediaType> current;
    HRESULT hr = reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current);
    if (FAILED(hr)) {
        std::cerr << "[PlayerCore] GetCurrentMediaType failed hr=" << hr_to_string(hr) << std::endl;
        return false;
    }
    std::string tag = reason ? std::string(reason) : std::string("reader-type");
    log_media_type(tag.c_str(), current.Get());

    UINT32 sr = fmt_.sr;
    UINT32 ch = fmt_.ch;
    UINT32 bps = fmt_.bps;
    current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    current->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
    GUID subtype{};
    current->GetGUID(MF_MT_SUBTYPE, &subtype);

    fmt_.sr = sr;
    fmt_.ch = ch;
    readerSubtype_ = subtype;
    readerBitsPerSample_ = bps ? bps : 16;
    readerBytesPerFrame_ = fmt_.ch * ((readerBitsPerSample_ + 7) / 8);
    if (readerBytesPerFrame_ == 0) {
        readerBytesPerFrame_ = fmt_.ch * (fmt_.bps / 8);
    }
    convertFloatToS16_ = IsEqualGUID(subtype, MFAudioFormat_Float);
    downmixIntToS16_ = (!convertFloatToS16_ && readerBitsPerSample_ != 16);
    fmt_.bps = (convertFloatToS16_ || downmixIntToS16_) ? 16u : readerBitsPerSample_;
    if (convertFloatToS16_) {
        std::cerr << "[PlayerCore] Reader delivers float32 PCM; enabling float->s16 conversion" << std::endl;
    } else if (downmixIntToS16_) {
        std::cerr << "[PlayerCore] Reader delivers PCM " << readerBitsPerSample_ << " bits; downmixing to s16" << std::endl;
    }
    return true;
}

void PlayerCore::convert_samples_to_s16(const uint8_t* src, size_t frames){
    const size_t sampleCount = frames * fmt_.ch;
    scratch16_.resize(sampleCount);
    if (convertFloatToS16_) {
        const float* fsrc = reinterpret_cast<const float*>(src);
        for (size_t i=0; i<sampleCount; ++i) {
            float v = std::clamp(fsrc[i], -1.0f, 1.0f);
            scratch16_[i] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
        }
        return;
    }
    if (!downmixIntToS16_) {
        const int16_t* ssrc = reinterpret_cast<const int16_t*>(src);
        std::copy(ssrc, ssrc + sampleCount, scratch16_.begin());
        return;
    }
    if (readerBitsPerSample_ == 24) {
        const uint8_t* bytes = src;
        for (size_t i=0; i<sampleCount; ++i) {
            int32_t value = (static_cast<int32_t>(bytes[0]) |
                            (static_cast<int32_t>(bytes[1]) << 8) |
                            (static_cast<int32_t>(bytes[2]) << 16));
            if (value & 0x800000) value |= ~0xFFFFFF;
            scratch16_[i] = static_cast<int16_t>(value >> 8);
            bytes += 3;
        }
        return;
    }
    if (readerBitsPerSample_ >= 32) {
        const int32_t* isrc = reinterpret_cast<const int32_t*>(src);
        for (size_t i=0; i<sampleCount; ++i) {
            scratch16_[i] = static_cast<int16_t>(isrc[i] >> 16);
        }
        return;
    }
    const int16_t* fallback = reinterpret_cast<const int16_t*>(src);
    std::copy(fallback, fallback + sampleCount, scratch16_.begin());
}

bool PlayerCore::needs_sample_conversion() const{
    return convertFloatToS16_ || downmixIntToS16_;
}

void PlayerCore::log_feed_stats(size_t frames, bool writeOk){
    framesFedSinceLog_ += frames;
    if (!writeOk) failedWritesSinceLog_++;
    auto now = std::chrono::steady_clock::now();
    if (feedLogStart_.time_since_epoch().count() == 0) {
        feedLogStart_ = now;
        return;
    }
    auto elapsed = now - feedLogStart_;
    if (elapsed >= std::chrono::seconds(1)) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cerr << "[PlayerCore] pushed " << framesFedSinceLog_ << " frames in " << ms
                  << "ms writeFail=" << failedWritesSinceLog_
                  << " paused=" << paused_.load() << std::endl;
        feedLogStart_ = now;
        framesFedSinceLog_ = 0;
        failedWritesSinceLog_ = 0;
    }
}
void PlayerCore::worker_loop(){
    while (!stop_.load()) {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]{ return stop_.load() || !paused_.load(); });
            if (stop_.load()) break;
        }

        DWORD idx = 0, flags = 0; LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &idx, &flags, &ts, &sample);
        if (FAILED(hr)) {
            std::cerr << "[PlayerCore] ReadSample failed hr=" << hr_to_string(hr) << std::endl;
            paused_.store(true);
            continue;
        }

        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if (!refresh_reader_media_type("reader-type-changed")) {
                std::cerr << "[PlayerCore] Failed to refresh media type change" << std::endl;
            }
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            std::cerr << "[PlayerCore] stream tick (audio gap) ts=" << ts << std::endl;
            if (!sample) continue;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            std::cerr << "[PlayerCore] end of stream" << std::endl;
            paused_.store(true);
            continue;
        }
        if (!sample) continue;

        last_pts_100ns_.store(ts);

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;

        BYTE* p = nullptr;
        DWORD cb = 0;
        if (FAILED(buf->Lock(&p, nullptr, &cb))) continue;

        if (readerBytesPerFrame_ == 0) {
            std::cerr << "[PlayerCore] readerBytesPerFrame_ is zero, skipping buffer" << std::endl;
            buf->Unlock();
            continue;
        }

        if (cb % readerBytesPerFrame_ != 0) {
            std::cerr << "[PlayerCore] sample size " << cb << " not aligned to frame size "
                      << readerBytesPerFrame_ << std::endl;
        }

        size_t frames = cb / readerBytesPerFrame_;
        if (frames == 0) {
            buf->Unlock();
            continue;
        }

        const void* payload = nullptr;
        if (needs_sample_conversion()) {
            convert_samples_to_s16(reinterpret_cast<const uint8_t*>(p), frames);
            payload = scratch16_.data();
        } else {
            payload = p;
        }

                bool writeOk = false;
        if (!bridge_) {
            std::cerr << "[PlayerCore] DualOut bridge missing; dropping " << frames << " frames" << std::endl;
        } else {
            writeOk = bridge_->eng.write(payload, frames, ts);
            log_feed_stats(frames, writeOk);
            if (!writeOk) {
                std::cerr << "[PlayerCore] DualOutEngine::write returned false" << std::endl;
            }
        }

        buf->Unlock();

        // Пейсинг по очереди
        if (bridge_) {
            while (!stop_.load() && !paused_.load()) {
                int qA = bridge_->eng.queueMsA();
                int qB = bridge_->eng.queueMsB();
                int qMin = (std::min)(qA, qB);

                if (qMin < 250)
                    break;

                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
}



bool PlayerCore::build_video_session(){
      lastHr_ = 0; lastErr_.clear();
    if (!hwnd_ || url_.empty()) { lastErr_="no hwnd or url"; return false; }

    // Создаём источник из URL
    Microsoft::WRL::ComPtr<IMFSourceResolver> resolver;
    HRESULT hr = MFCreateSourceResolver(&resolver);
    if (FAILED(hr)) { lastHr_=hr; lastErr_="MFCreateSourceResolver"; return false; }

    MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;
    Microsoft::WRL::ComPtr<IUnknown> srcUnk;

    hr = resolver->CreateObjectFromURL(
        url_.c_str(),
        MF_RESOLUTION_MEDIASOURCE,
        nullptr,
        &objType,
        &srcUnk
    );
     if (FAILED(hr)) { lastHr_=hr; lastErr_="CreateObjectFromURL"; return false; }
    srcUnk.As(&vSource_);

    // Сессия
      hr = MFCreateMediaSession(nullptr, &vSession_);
      if (FAILED(hr)) { lastHr_=hr; lastErr_="MFCreateMediaSession"; return false; }

    // Презентационный дескриптор
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd;
    hr = vSource_->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) { lastHr_=hr; lastErr_="CreatePresentationDescriptor"; return false; }

    // Найдём видеопоток
    DWORD streamCount = 0;
    hr = pd->GetStreamDescriptorCount(&streamCount);
     if (FAILED(hr)) { lastHr_=hr; lastErr_="CreateObjectFromURL"; return false; }

    DWORD vIndex = (DWORD)-1;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> vSD;
    for (DWORD i=0;i<streamCount;i++){
        BOOL sel = FALSE;
        Microsoft::WRL::ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(i, &sel, &sd);
        if (FAILED(hr)) continue;

        Microsoft::WRL::ComPtr<IMFMediaTypeHandler> th;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&th))) {
            GUID major{};
            if (SUCCEEDED(th->GetMajorType(&major)) && major==MFMediaType_Video) {
                vIndex = i; vSD = sd; pd->SelectStream(i); break;
            }
        }
    }
    if (vIndex==(DWORD)-1) return false; // видео нет

    // EVR sink
    Microsoft::WRL::ComPtr<IMFActivate> actEVR;
    hr = MFCreateVideoRendererActivate(hwnd_, &actEVR);
     if (FAILED(hr)) { lastHr_=hr; lastErr_="CreateObjectFromURL"; return false; }

    Microsoft::WRL::ComPtr<IMFTopology> topo;
    hr = MFCreateTopology(&topo);
    if (FAILED(hr)) { lastHr_=hr; lastErr_="MFCreateTopology"; return false; }
    // Узел источника
    Microsoft::WRL::ComPtr<IMFTopologyNode> srcNode;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &srcNode);
    if (FAILED(hr)) { lastHr_=hr; lastErr_="MFCreateTopologyNode(source)"; return false; }
    srcNode->SetUnknown(MF_TOPONODE_SOURCE, vSource_.Get());
    srcNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pd.Get());
    srcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, vSD.Get());
    topo->AddNode(srcNode.Get());

    // Узел приёмника (EVR)
    Microsoft::WRL::ComPtr<IMFTopologyNode> sinkNode;
    hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &sinkNode);
      if (FAILED(hr)) { lastHr_=hr; lastErr_="MFCreateTopologyNode(output)"; return false; }

    Microsoft::WRL::ComPtr<IMFMediaSink> sink;
    hr = actEVR->ActivateObject(IID_PPV_ARGS(&sink));
     if (FAILED(hr)) { lastHr_=hr; lastErr_="CreateObjectFromURL"; return false; }

    sinkNode->SetObject(sink.Get());
    topo->AddNode(sinkNode.Get());

    // Соединяем
    hr = srcNode->ConnectOutput(0, sinkNode.Get(), 0);
       if (FAILED(hr)) { lastHr_=hr; lastErr_="ConnectOutput"; return false; }

    // Готовим сессию
    hr = vSession_->SetTopology(0, topo.Get());
     if (FAILED(hr)) { lastHr_=hr; lastErr_="SetTopology"; return false; }

    videoReady_ = true;
    return true;
}

void PlayerCore::destroy_video_session(){
    if (vSession_) {
        vSession_->Close();
        vSession_.Reset();
    }
    vSource_.Reset();
}

bool PlayerCore::video_start(){
    if (!vSession_) return false;
    PROPVARIANT pos; PropVariantInit(&pos);
    pos.vt = VT_I8; pos.hVal.QuadPart = last_pts_100ns_.load(); // старт с текущей позиции аудио
    HRESULT hr = vSession_->Start(&GUID_NULL, &pos);
    PropVariantClear(&pos);
    return SUCCEEDED(hr);
}

bool PlayerCore::video_pause(){
    return vSession_ ? SUCCEEDED(vSession_->Pause()) : false;
}

bool PlayerCore::video_stop(){
    return vSession_ ? SUCCEEDED(vSession_->Stop()) : false;
}

bool PlayerCore::video_seek_100ns(LONGLONG pos){
    if (!vSession_) return false;
    PROPVARIANT var; PropVariantInit(&var);
    var.vt = VT_I8; var.hVal.QuadPart = pos;
    // Seek делается через Start c позицией
    HRESULT hr = vSession_->Start(&GUID_NULL, &var);
    PropVariantClear(&var);
    return SUCCEEDED(hr);
}
