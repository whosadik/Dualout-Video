#pragma once
#include <string>
#include <atomic>
#include "DualOutEngine.h"
#include "mf_audio_reader.h"

struct DualOutBridge {
  DualOutEngine eng;
  PcmDesc fmt{};
  std::atomic_bool stop{false};
  bool openDevices(const std::wstring& devA, const std::wstring& devB, const PcmDesc& f);
  bool playUrl(const std::wstring& url);
  void stopAll();
};
