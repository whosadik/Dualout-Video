#define NOMINMAX
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <thread>

#include "dualout_bridge.h"
#include "player_core.h"
#include <windows.h>


// trim
static inline void ltrim(std::string& s){ s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c){return !std::isspace(c);})); }
static inline void rtrim(std::string& s){ s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c){return !std::isspace(c);}).base(), s.end()); }
static inline void trim(std::string& s){ ltrim(s); rtrim(s); }
static std::wstring wfromu8(const std::string& s){
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if(n<=0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::unordered_map<std::string,std::string> parse_kv_line(const std::string& line){
    std::unordered_map<std::string,std::string> kv;
    size_t i=0, n=line.size();
    while(i<n){
        while(i<n && std::isspace((unsigned char)line[i])) ++i;
        if(i>=n) break;
        size_t ks=i;
        while(i<n && line[i]!='=' && !std::isspace((unsigned char)line[i])) ++i;
        std::string key = line.substr(ks, i-ks);
        while(i<n && std::isspace((unsigned char)line[i])) ++i;
        if(i<n && line[i]=='=') ++i;
        while(i<n && std::isspace((unsigned char)line[i])) ++i;
        std::string val;
        if(i<n && (line[i]=='"' || line[i]=='\'')){
            char q=line[i++]; size_t vs=i; while(i<n && line[i]!=q) ++i;
            val = line.substr(vs, i-vs); if(i<n && line[i]==q) ++i;
        } else {
            size_t vs=i; while(i<n && !std::isspace((unsigned char)line[i])) ++i;
            val = line.substr(vs, i-vs);
        }
        if(!key.empty()) kv[key]=val;
    }
    return kv;
}

static inline int dualout_core_list_devices_json(char*, int){ return 0; }


int main(){
    DualOutBridge bridge;
    PlayerCore player;

    std::wstring devA=L"", devB=L"";
    PcmDesc fmt{48000,2,16};

    std::string line;
    while(std::getline(std::cin, line)){
        trim(line);
        if(line.empty()) continue;

        auto kv = parse_kv_line(line);
        auto itCmd = kv.find("cmd");
        if(itCmd==kv.end()){ std::cout << R"({"ok":false,"err":"missing_cmd"})" << "\n"; continue; }
        const std::string cmd = itCmd->second;

                if (cmd == "set_devices") {
    std::string a = kv.count("a") ? kv["a"] : "";
    std::string b = kv.count("b") ? kv["b"] : "";

    devA = (a == "default") ? L"" : wfromu8(a);
    devB = (b == "default") ? L"" : wfromu8(b);

    // ЖЁСТКО: всегда 48000, игнорируем sr из команды
    fmt.sr  = 48000;
    fmt.ch  = kv.count("ch")  ? (uint32_t)std::stoul(kv["ch"])  : 2;
    fmt.bps = kv.count("bps") ? (uint32_t)std::stoul(kv["bps"]) : 16;

    bool ok = bridge.openDevices(devA, devB, fmt);
    std::cout << (ok ? R"({"ok":true})" : R"({"ok":false})") << "\n";
}

        else if(cmd=="open"){
            std::wstring url = wfromu8(kv.count("url")? kv["url"] : "");
            bool ok = player.open(url, &bridge);
            std::cout << (ok? R"({"ok":true})" : R"({"ok":false})") << "\n";
        }
        else if(cmd=="play"){
            std::cout << (player.play()? R"({"ok":true})" : R"({"ok":false})") << "\n";
        }
        else if(cmd=="pause"){
            std::cout << (player.pause()? R"({"ok":true})" : R"({"ok":false})") << "\n";
        }
        else if(cmd=="seek"){
            long long ms = kv.count("ms")? std::stoll(kv["ms"]) : 0;
            std::cout << (player.seek_ms(ms)? R"({"ok":true})" : R"({"ok":false})") << "\n";
        }
        else if (cmd == "set_volume") {
            // a_db / b_db оставляем на будущее, пока можно всегда 0
            float aDb    = kv.count("a_db")    ? std::stof(kv["a_db"])    : 0.0f;
            float bDb    = kv.count("b_db")    ? std::stof(kv["b_db"])    : 0.0f;
            float master = kv.count("master")  ? std::stof(kv["master"])  : 1.0f;

            bridge.eng.setGainDb(aDb, bDb, master);
            std::cout << R"({"ok":true})" << "\n";
        }
        // НОВОЕ: тестовый тон
        else if(cmd=="test_tone"){
            int durMs = kv.count("ms") ? std::stoi(kv["ms"]) : 3000;   // длительность, по умолчанию 3 сек
            double freq = kv.count("freq") ? std::stod(kv["freq"]) : 440.0; // частота, по умолчанию 440 Гц

            const uint32_t sr = fmt.sr;
            const uint32_t ch = fmt.ch;
            if (sr == 0 || ch == 0) {
                std::cout << R"({"ok":false,"err":"bad_format"})" << "\n";
            } else {
                size_t frames = (size_t)sr * durMs / 1000;
                std::vector<int16_t> buf(frames * ch);

                const double twoPiF = 2.0 * 3.14159265358979323846 * freq;
                for(size_t i=0; i<frames; ++i){
                    double t = (double)i / (double)sr;
                    double s = std::sin(twoPiF * t);
                    int16_t sample = (int16_t)(s * 3000); // не в полную громкость
                    for(uint32_t c=0; c<ch; ++c){
                        buf[i*ch + c] = sample;
                    }
                }

                bool ok = bridge.eng.write(buf.data(), frames, 0);
                std::cout << (ok ? R"({"ok":true})" : R"({"ok":false})") << "\n";
            }
        }
        else if(cmd=="set_hwnd"){
            auto s = kv.count("hwnd") ? kv["hwnd"] : "";
            auto parseHWND = [](const std::string& t)->HWND{
                if(t.empty()) return nullptr;
                unsigned long long v=0;
                if(t.rfind("0x",0)==0 || t.rfind("0X",0)==0) {
                    v = std::stoull(t, nullptr, 16);
                } else {
                    v = std::stoull(t, nullptr, 10);
                }
                return (HWND)(uintptr_t)v;
            };

            HWND h = parseHWND(s);
            bool ok = player.set_hwnd(h);
            if(ok){
                std::cout << R"({"ok":true})" << "\n";
            } else {
                std::cout << "{\"ok\":false,\"err\":\"" << player.last_err()
                          << "\",\"hr\":\"0x" << std::hex << player.last_hr() << "\"}\n";
                std::cout << std::dec;
            }
        }
               else if (cmd == "set_swap") {
            bool v = false;
            if (kv.count("v")) {
                std::string s = kv["v"];
                std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                v = (s == "1" || s == "true" || s == "yes");
            }
            bridge.eng.setSwapLR(v);
            std::cout << R"({"ok":true})" << "\n";
        }

        // НОВОЕ: запрос уровней сигнала (RMS/PEAK)
        else if (cmd == "levels") {
            float rmsL = 0.0f;
            float rmsR = 0.0f;
            float peakL = 0.0f;
            float peakR = 0.0f;

            // ЭТО мы реализуем в следующем файле (движок):
            // bool DualOutEngine::getLevels(float& rmsL, float& rmsR, float& peakL, float& peakR);
            bool ok = bridge.eng.getLevels(rmsL, rmsR, peakL, peakR);

            if (!ok) {
                std::cout << R"({"ok":false,"err":"no_levels"})" << "\n";
            } else {
                std::cout
                    << "{\"ok\":true"
                    << ",\"rmsL\":"  << rmsL
                    << ",\"rmsR\":"  << rmsR
                    << ",\"peakL\":" << peakL
                    << ",\"peakR\":" << peakR
                    << "}\n";
            }
        }

        else if(cmd=="status"){
            std::cout << player.status_json() << "\n";
        }
        else if(cmd=="stop"){
            bool a = player.stop();
            bridge.stopAll();
            std::cout << (a? R"({"ok":true})" : R"({"ok":false})") << "\n";
        }
        else{
            std::cout << R"({"ok":false,"err":"unknown_cmd"})" << "\n";
        }

    }
    return 0;
}
