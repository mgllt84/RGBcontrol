#pragma once

#include <memory>
#include <string>

struct AudioBands {
    double volume = 0.0;
    double bass = 0.0;
    double mid = 0.0;
    double treble = 0.0;
    bool signal = false;
};

class LoopbackAudioAnalyzer {
public:
    LoopbackAudioAnalyzer();
    ~LoopbackAudioAnalyzer();
    LoopbackAudioAnalyzer(const LoopbackAudioAnalyzer&) = delete;
    LoopbackAudioAnalyzer& operator=(const LoopbackAudioAnalyzer&) = delete;

    bool start(std::wstring& error);
    bool sample(AudioBands& bands, std::wstring& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
