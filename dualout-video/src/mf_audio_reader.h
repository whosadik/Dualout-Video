#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include <atomic>

struct PcmDesc { uint32_t sr, ch, bps; };
using PcmCallback = std::function<void(const void* data, size_t frames, int64_t pts100ns)>;

bool mf_stream_audio_pcm(const std::wstring& url, const PcmCallback& onPcm,
                         PcmDesc* outFmt=nullptr, std::atomic_bool* stopFlag=nullptr);
