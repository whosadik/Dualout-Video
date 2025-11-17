#include "mf_utils.h"
#include "mf_audio_reader.h"
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void setAudioPcm(ComPtr<IMFSourceReader>& r, PcmDesc& fmt){
  ComPtr<IMFMediaType> out;
  MF_THROW(MFCreateMediaType(&out));
  MF_THROW(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
  MF_THROW(out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
  MF_THROW(out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, fmt.sr));
  MF_THROW(out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, fmt.ch));
  MF_THROW(out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, fmt.bps));
  MF_THROW(out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (fmt.ch * (fmt.bps/8))));
  MF_THROW(out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, fmt.sr * fmt.ch * (fmt.bps/8)));
  MF_THROW(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, out.Get()));
}

bool mf_stream_audio_pcm(const std::wstring& url, const PcmCallback& onPcm,
                         PcmDesc* outFmt, std::atomic_bool* stopFlag){
  ComInit com; MFInit mf;
  ComPtr<IMFAttributes> attr; MF_THROW(MFCreateAttributes(&attr, 2));
  MF_THROW(attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
  MF_THROW(attr->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE));

  ComPtr<IMFSourceReader> r;
  MF_THROW(MFCreateSourceReaderFromURL(url.c_str(), attr.Get(), &r));

  // запрашиваем нативный тип, чтобы знать что приходит
  ComPtr<IMFMediaType> native; MF_THROW(r->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native));
  UINT32 sr=48000, ch=2, bps=16;
  native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
  native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
  native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);

  PcmDesc pcm{sr, ch, 16}; // для MVP приводим к 16 бит
  setAudioPcm(r, pcm);
  if(outFmt) *outFmt = pcm;

  while(true){
    if(stopFlag && stopFlag->load()) return true;
    DWORD streamIndex=0, flags=0; LONGLONG ts=0; ComPtr<IMFSample> sample;
    HRESULT hr = r->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &ts, &sample);
    if(FAILED(hr)) return false;
    if(flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if(!sample) continue;

    ComPtr<IMFMediaBuffer> buf; MF_THROW(sample->ConvertToContiguousBuffer(&buf));
    BYTE* p=nullptr; DWORD cb=0; MF_THROW(buf->Lock(&p, nullptr, &cb));
    size_t frames = cb / (pcm.ch * (pcm.bps/8));
    onPcm(p, frames, ts);
    buf->Unlock();
  }
  return true;
}
