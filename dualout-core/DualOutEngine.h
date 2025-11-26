#pragma once
#include <string>
#include <cstdint>

struct DualOutFormat { uint32_t sr, ch, bps; };

class DualOutEngine {
public:
  bool init(const std::wstring& devA, const std::wstring& devB, DualOutFormat fmt, bool exclusive=false);
  bool write(const void* pcmInterleaved, size_t frames, int64_t pts100ns);

  void setDelayMs(int a, int b);
  void setGainDb(float a, float b, float master);

  void drain();
  void stop();
  void flush(); // НОВОЕ

  int queueMsA() const;
  int queueMsB() const;
  int driftMsAB() const;

  // === NEW: reverse stereo channels ===
  void setSwapLR(bool v);
};

