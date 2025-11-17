#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <combaseapi.h>
#include <stdexcept>

inline void MF_THROW(HRESULT hr){ if(FAILED(hr)) throw std::runtime_error("MF error"); }
struct ComInit { ComInit(){ MF_THROW(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
                 ~ComInit(){ CoUninitialize(); } };
struct MFInit  { MFInit(){ MF_THROW(MFStartup(MF_VERSION)); }
                 ~MFInit(){ MFShutdown(); } };
