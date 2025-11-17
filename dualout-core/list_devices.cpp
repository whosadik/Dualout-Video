#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <iostream>
#include <windows.h>

int main(){
    // Консоль в UTF-8, чтобы имена были нормальными
    SetConsoleOutputCP(CP_UTF8);

    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) {
        std::cerr << "context init failed\n";
        return 1;
    }

    ma_device_info* pPlaybackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(&ctx, &pPlaybackInfos, &playbackCount, NULL, NULL) != MA_SUCCESS) {
        std::cerr << "get_devices failed\n";
        ma_context_uninit(&ctx);
        return 1;
    }

    std::cout << "Playback devices:\n";
    for (ma_uint32 i = 0; i < playbackCount; ++i){
        std::cout << "[" << i << "] " << pPlaybackInfos[i].name << "\n";
    }

    ma_context_uninit(&ctx);
    return 0;
}
