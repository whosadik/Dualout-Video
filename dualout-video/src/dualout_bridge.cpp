#include "dualout_bridge.h"

bool DualOutBridge::openDevices(const std::wstring& a, const std::wstring& b, const PcmDesc& f){
  fmt = f;
  DualOutFormat df{f.sr, f.ch, f.bps};
  return eng.init(a, b, df, false);
}

bool DualOutBridge::playUrl(const std::wstring& url){
  stop.store(false);
  return mf_stream_audio_pcm(url,
    [this](const void* data, size_t frames, int64_t pts){
      eng.write(data, frames, pts);
    }, &fmt, &stop);
}

void DualOutBridge::stopAll(){
  stop.store(true);
  eng.drain();
  eng.stop();
}
