
// WavVoiceShaper.cpp
// 汎用WAV成形コンソールアプリケーション
// 入力WAVを読み込み、音量正規化・ゲイン・DC補正・無音削除・ノイズ抑制・ボイスチェンジャー・フェード・簡易EQ・コンプレッサー・リミッターを適用して出力します。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <utility>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include("world/dio.h") && __has_include("world/stonemask.h") && __has_include("world/cheaptrick.h") && __has_include("world/d4c.h") && __has_include("world/synthesis.h")
#define WVS_HAS_WORLD 1
#include "world/dio.h"
#include "world/stonemask.h"
#include "world/cheaptrick.h"
#include "world/d4c.h"
#include "world/synthesis.h"
#else
#define WVS_HAS_WORLD 0
#endif

namespace
{
const int EXIT_OK = 0;
const int EXIT_ARGUMENT_ERROR = 1;
const int EXIT_INPUT_NOT_FOUND = 2;
const int EXIT_READ_FAILED = 3;
const int EXIT_PROCESS_FAILED = 4;
const int EXIT_WRITE_FAILED = 5;

struct SOptions
{
    std::wstring inPath;
    std::wstring outPath;
    std::wstring configPath;
    std::wstring presetName;
    std::vector<std::pair<std::wstring, std::wstring> > overrides;
    bool showHelp = false;
};

struct SEffectSettings
{
    bool normalize = false;
    double targetPeakDb = -1.0;
    double preGainDb = 0.0;
    double outputGainDb = 0.0;
    bool removeDcOffset = false;

    bool trimSilence = false;
    double trimThresholdDb = -45.0;
    int trimPaddingMs = 80;

    int fadeInMs = 0;
    int fadeOutMs = 0;

    bool limiter = false;
    double limiterThresholdDb = -1.0;

    bool compressor = false;
    double compressorThresholdDb = -16.0;
    double compressorRatio = 2.0;

    double eqLowGainDb = 0.0;
    double eqMidGainDb = 0.0;
    double eqHighGainDb = 0.0;
    double eqLowCutHz = 250.0;
    double eqHighCutHz = 3500.0;

    bool noiseGate = false;
    double noiseGateThresholdDb = -55.0;
    double noiseGateFloorDb = -80.0;

    bool voiceChanger = false;
    std::wstring voiceChangerMode = L"None";
    std::wstring voiceChangerEngine = L"Builtin";
    double voiceChangeAmount = 70.0;
    double f0Scale = 1.0;
    double aperiodicityScale = 1.0;
    double robotPitchFlatten = 0.0;
    double worldFramePeriodMs = 5.0;
    double worldF0FloorHz = 71.0;
    double worldF0CeilHz = 800.0;
    double pitchShiftSemitone = 0.0;
    double formantShiftRatio = 1.0;
    double tempoRatio = 1.0;
    double dryWet = 100.0;
    double robotAmount = 0.0;
    double ringModHz = 35.0;
};

struct SWavFormat
{
    WORD audioFormat = 0;
    WORD channels = 0;
    DWORD sampleRate = 0;
    DWORD byteRate = 0;
    WORD blockAlign = 0;
    WORD bitsPerSample = 0;
    std::vector<unsigned char> fmtBytes;
};

struct SWavData
{
    SWavFormat format;
    std::vector<short> samples;
};

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    size_t len = std::wcslen(prefix);
    return value.size() >= len && _wcsnicmp(value.c_str(), prefix, len) == 0;
}

bool IsAbsolutePath(const std::wstring& path)
{
    if (path.size() >= 2 && path[1] == L':') return true;
    if (StartsWith(path, L"\\\\")) return true;
    return false;
}

std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return s.substr(0, pos);
}

std::wstring ResolvePath(const std::wstring& path)
{
    if (path.empty()) return path;
    if (IsAbsolutePath(path)) return path;
    return GetExeDirectory() + L"\\" + path;
}

bool FileExists(const std::wstring& path)
{
    DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>& data)
{
    data.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 1024LL * 1024LL * 1024LL)
    {
        ::CloseHandle(h);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD totalRead = 0;
    DWORD read = 0;
    while (totalRead < data.size())
    {
        DWORD request = static_cast<DWORD>(std::min<size_t>(data.size() - totalRead, 1024 * 1024));
        if (!::ReadFile(h, data.data() + totalRead, request, &read, nullptr))
        {
            ::CloseHandle(h);
            return false;
        }
        if (read == 0) break;
        totalRead += read;
    }
    ::CloseHandle(h);

    if (totalRead != data.size())
    {
        data.resize(totalRead);
    }
    return true;
}

bool WriteAllBytes(const std::wstring& path, const std::vector<unsigned char>& data)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD totalWritten = 0;
    while (totalWritten < data.size())
    {
        DWORD written = 0;
        DWORD request = static_cast<DWORD>(std::min<size_t>(data.size() - totalWritten, 1024 * 1024));
        if (!::WriteFile(h, data.data() + totalWritten, request, &written, nullptr))
        {
            ::CloseHandle(h);
            return false;
        }
        if (written == 0) break;
        totalWritten += written;
    }
    ::CloseHandle(h);
    return totalWritten == data.size();
}

DWORD ReadLe32(const std::vector<unsigned char>& b, size_t pos)
{
    if (pos + 4 > b.size()) return 0;
    return static_cast<DWORD>(b[pos]) |
        (static_cast<DWORD>(b[pos + 1]) << 8) |
        (static_cast<DWORD>(b[pos + 2]) << 16) |
        (static_cast<DWORD>(b[pos + 3]) << 24);
}

WORD ReadLe16(const std::vector<unsigned char>& b, size_t pos)
{
    if (pos + 2 > b.size()) return 0;
    return static_cast<WORD>(b[pos] | (b[pos + 1] << 8));
}

void WriteFourCc(std::vector<unsigned char>& out, const char* id)
{
    out.push_back(static_cast<unsigned char>(id[0]));
    out.push_back(static_cast<unsigned char>(id[1]));
    out.push_back(static_cast<unsigned char>(id[2]));
    out.push_back(static_cast<unsigned char>(id[3]));
}

void WriteLe16(std::vector<unsigned char>& out, WORD value)
{
    out.push_back(static_cast<unsigned char>(value & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
}

void WriteLe32(std::vector<unsigned char>& out, DWORD value)
{
    out.push_back(static_cast<unsigned char>(value & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

bool ParseWav(const std::vector<unsigned char>& bytes, SWavData& wav)
{
    if (bytes.size() < 44) return false;
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0) return false;
    if (std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;

    bool foundFmt = false;
    bool foundData = false;
    size_t dataPos = 0;
    DWORD dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        char id[5] = { 0 };
        std::memcpy(id, bytes.data() + pos, 4);
        DWORD size = ReadLe32(bytes, pos + 4);
        size_t body = pos + 8;
        if (body + size > bytes.size()) break;

        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            if (size < 16) return false;
            wav.format.fmtBytes.assign(bytes.begin() + body, bytes.begin() + body + size);
            wav.format.audioFormat = ReadLe16(bytes, body + 0);
            wav.format.channels = ReadLe16(bytes, body + 2);
            wav.format.sampleRate = ReadLe32(bytes, body + 4);
            wav.format.byteRate = ReadLe32(bytes, body + 8);
            wav.format.blockAlign = ReadLe16(bytes, body + 12);
            wav.format.bitsPerSample = ReadLe16(bytes, body + 14);
            foundFmt = true;
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            dataPos = body;
            dataSize = size;
            foundData = true;
        }

        pos = body + size + (size & 1);
    }

    if (!foundFmt || !foundData) return false;
    if (wav.format.audioFormat != 1 || wav.format.bitsPerSample != 16 || wav.format.channels == 0 || wav.format.blockAlign == 0)
    {
        return false;
    }

    size_t sampleCount = dataSize / sizeof(short);
    wav.samples.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i)
    {
        size_t p = dataPos + i * 2;
        WORD u = ReadLe16(bytes, p);
        wav.samples[i] = static_cast<short>(u);
    }
    return true;
}

std::vector<unsigned char> BuildWavBytes(const SWavData& wav)
{
    std::vector<unsigned char> data;
    data.reserve(wav.samples.size() * sizeof(short));
    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        short s = wav.samples[i];
        WriteLe16(data, static_cast<WORD>(s));
    }

    std::vector<unsigned char> fmt = wav.format.fmtBytes;
    if (fmt.size() < 16)
    {
        fmt.clear();
        WriteLe16(fmt, 1);
        WriteLe16(fmt, wav.format.channels);
        WriteLe32(fmt, wav.format.sampleRate);
        WriteLe32(fmt, wav.format.byteRate);
        WriteLe16(fmt, wav.format.blockAlign);
        WriteLe16(fmt, wav.format.bitsPerSample);
    }

    DWORD riffSize = static_cast<DWORD>(4 + 8 + fmt.size() + 8 + data.size());
    std::vector<unsigned char> out;
    out.reserve(static_cast<size_t>(riffSize) + 8);
    WriteFourCc(out, "RIFF");
    WriteLe32(out, riffSize);
    WriteFourCc(out, "WAVE");
    WriteFourCc(out, "fmt ");
    WriteLe32(out, static_cast<DWORD>(fmt.size()));
    out.insert(out.end(), fmt.begin(), fmt.end());
    if (fmt.size() & 1) out.push_back(0);
    WriteFourCc(out, "data");
    WriteLe32(out, static_cast<DWORD>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

double DbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

double ClampDouble(double v, double minValue, double maxValue)
{
    if (v < minValue) return minValue;
    if (v > maxValue) return maxValue;
    return v;
}

int ClampInt(int v, int minValue, int maxValue)
{
    if (v < minValue) return minValue;
    if (v > maxValue) return maxValue;
    return v;
}

double SampleToDouble(short s)
{
    return static_cast<double>(s) / 32768.0;
}

short DoubleToSample(double v)
{
    v = ClampDouble(v, -1.0, 1.0);
    int iv = static_cast<int>(std::lround(v * 32767.0));
    if (iv < -32768) iv = -32768;
    if (iv > 32767) iv = 32767;
    return static_cast<short>(iv);
}

void TrimSilence(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.trimSilence || wav.samples.empty() || wav.format.channels == 0) return;

    const int channels = wav.format.channels;
    const size_t frameCount = wav.samples.size() / channels;
    if (frameCount == 0) return;

    const double threshold = DbToLinear(settings.trimThresholdDb);
    size_t first = frameCount;
    size_t last = 0;

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        double maxAbs = 0.0;
        for (int ch = 0; ch < channels; ++ch)
        {
            maxAbs = (std::max)(maxAbs, std::fabs(SampleToDouble(wav.samples[frame * channels + ch])));
        }
        if (maxAbs >= threshold)
        {
            if (first == frameCount) first = frame;
            last = frame;
        }
    }

    if (first == frameCount) return;

    size_t padding = static_cast<size_t>((static_cast<unsigned long long>(wav.format.sampleRate) * static_cast<unsigned int>((std::max)(0, settings.trimPaddingMs))) / 1000ULL);
    first = (first > padding) ? first - padding : 0;
    last = (std::min)(frameCount - 1, last + padding);
    if (last <= first) return;

    std::vector<short> trimmed;
    trimmed.reserve((last - first + 1) * channels);
    trimmed.insert(trimmed.end(), wav.samples.begin() + first * channels, wav.samples.begin() + (last + 1) * channels);
    wav.samples.swap(trimmed);
}

void ApplyFade(SWavData& wav, const SEffectSettings& settings)
{
    if (wav.samples.empty() || wav.format.channels == 0) return;

    const int channels = wav.format.channels;
    const size_t frameCount = wav.samples.size() / channels;
    if (frameCount == 0) return;

    size_t fadeInFrames = static_cast<size_t>((static_cast<unsigned long long>(wav.format.sampleRate) * static_cast<unsigned int>((std::max)(0, settings.fadeInMs))) / 1000ULL);
    size_t fadeOutFrames = static_cast<size_t>((static_cast<unsigned long long>(wav.format.sampleRate) * static_cast<unsigned int>((std::max)(0, settings.fadeOutMs))) / 1000ULL);
    fadeInFrames = (std::min)(fadeInFrames, frameCount);
    fadeOutFrames = (std::min)(fadeOutFrames, frameCount);

    for (size_t frame = 0; frame < fadeInFrames; ++frame)
    {
        double gain = fadeInFrames > 0 ? static_cast<double>(frame) / static_cast<double>(fadeInFrames) : 1.0;
        for (int ch = 0; ch < channels; ++ch)
        {
            size_t i = frame * channels + ch;
            wav.samples[i] = DoubleToSample(SampleToDouble(wav.samples[i]) * gain);
        }
    }

    for (size_t frame = 0; frame < fadeOutFrames; ++frame)
    {
        double gain = fadeOutFrames > 0 ? static_cast<double>(fadeOutFrames - frame) / static_cast<double>(fadeOutFrames) : 1.0;
        size_t targetFrame = frameCount - 1 - frame;
        for (int ch = 0; ch < channels; ++ch)
        {
            size_t i = targetFrame * channels + ch;
            wav.samples[i] = DoubleToSample(SampleToDouble(wav.samples[i]) * gain);
        }
    }
}

void ApplyGain(SWavData& wav, double gainDb)
{
    if (wav.samples.empty() || std::fabs(gainDb) < 0.01) return;

    double gain = ClampDouble(DbToLinear(gainDb), 0.01, 100.0);
    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        wav.samples[i] = DoubleToSample(SampleToDouble(wav.samples[i]) * gain);
    }
}

void RemoveDcOffset(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.removeDcOffset || wav.samples.empty() || wav.format.channels == 0) return;

    const int channels = wav.format.channels;
    const size_t frameCount = wav.samples.size() / channels;
    if (frameCount == 0) return;

    std::vector<double> sums(channels, 0.0);
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            sums[ch] += SampleToDouble(wav.samples[frame * channels + ch]);
        }
    }

    for (int ch = 0; ch < channels; ++ch)
    {
        sums[ch] /= static_cast<double>(frameCount);
    }

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            size_t i = frame * channels + ch;
            wav.samples[i] = DoubleToSample(SampleToDouble(wav.samples[i]) - sums[ch]);
        }
    }
}

void ApplyNoiseGate(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.noiseGate || wav.samples.empty()) return;

    double threshold = ClampDouble(DbToLinear(settings.noiseGateThresholdDb), 0.000001, 0.99);
    double floorGain = ClampDouble(DbToLinear(settings.noiseGateFloorDb), 0.0, 1.0);
    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        double x = SampleToDouble(wav.samples[i]);
        if (std::fabs(x) < threshold)
        {
            wav.samples[i] = DoubleToSample(x * floorGain);
        }
    }
}

void ApplyEq(SWavData& wav, const SEffectSettings& settings)
{
    if (wav.samples.empty() || wav.format.channels == 0) return;
    if (std::fabs(settings.eqLowGainDb) < 0.01 && std::fabs(settings.eqMidGainDb) < 0.01 && std::fabs(settings.eqHighGainDb) < 0.01) return;

    const int channels = wav.format.channels;
    const size_t frameCount = wav.samples.size() / channels;
    if (frameCount == 0) return;

    const double sampleRate = static_cast<double>(std::max<DWORD>(wav.format.sampleRate, 8000));
    const double maxHz = (std::max)(100.0, sampleRate * 0.45);
    const double lowHz = ClampDouble(settings.eqLowCutHz, 20.0, (std::max)(20.0, maxHz - 50.0));
    const double highHz = ClampDouble(settings.eqHighCutHz, lowHz + 50.0, maxHz);
    const double pi = 3.14159265358979323846;
    const double alphaLow = 1.0 - std::exp(-2.0 * pi * lowHz / sampleRate);
    const double alphaHighCut = 1.0 - std::exp(-2.0 * pi * highHz / sampleRate);
    const double lowGain = DbToLinear(settings.eqLowGainDb);
    const double midGain = DbToLinear(settings.eqMidGainDb);
    const double highGain = DbToLinear(settings.eqHighGainDb);

    std::vector<double> lowState(channels, 0.0);
    std::vector<double> highCutState(channels, 0.0);

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            size_t i = frame * channels + ch;
            double x = SampleToDouble(wav.samples[i]);
            lowState[ch] += alphaLow * (x - lowState[ch]);
            highCutState[ch] += alphaHighCut * (x - highCutState[ch]);

            double low = lowState[ch];
            double high = x - highCutState[ch];
            double mid = highCutState[ch] - low;
            double y = low * lowGain + mid * midGain + high * highGain;
            wav.samples[i] = DoubleToSample(y);
        }
    }
}

void ApplyCompressor(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.compressor || wav.samples.empty()) return;

    double threshold = DbToLinear(settings.compressorThresholdDb);
    threshold = ClampDouble(threshold, 0.001, 0.99);
    double ratio = ClampDouble(settings.compressorRatio, 1.0, 20.0);

    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        double x = SampleToDouble(wav.samples[i]);
        double a = std::fabs(x);
        if (a > threshold)
        {
            double compressed = threshold * std::pow(a / threshold, 1.0 / ratio);
            x = (x < 0.0) ? -compressed : compressed;
            wav.samples[i] = DoubleToSample(x);
        }
    }
}

void ApplyNormalize(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.normalize || wav.samples.empty()) return;

    double peak = 0.0;
    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        peak = (std::max)(peak, std::fabs(SampleToDouble(wav.samples[i])));
    }
    if (peak < 0.000001) return;

    double target = ClampDouble(DbToLinear(settings.targetPeakDb), 0.01, 1.0);
    double gain = ClampDouble(target / peak, 0.05, 20.0);
    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        wav.samples[i] = DoubleToSample(SampleToDouble(wav.samples[i]) * gain);
    }
}

void ApplyLimiter(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.limiter || wav.samples.empty()) return;

    double threshold = ClampDouble(DbToLinear(settings.limiterThresholdDb), 0.01, 1.0);
    for (size_t i = 0; i < wav.samples.size(); ++i)
    {
        double x = SampleToDouble(wav.samples[i]);
        x = ClampDouble(x, -threshold, threshold);
        wav.samples[i] = DoubleToSample(x);
    }
}


std::wstring TrimWide(std::wstring value);
bool EqualsNoCaseWide(const std::wstring& a, const std::wstring& b);

struct SComplexValue
{
    double re;
    double im;
};

const double PI_VALUE = 3.1415926535897932384626433832795;

bool IsVoiceChangerMode(const std::wstring& mode, const wchar_t* a, const wchar_t* b = L"", const wchar_t* c = L"", const wchar_t* d = L"")
{
    return EqualsNoCaseWide(mode, a) || (b[0] != L'\0' && EqualsNoCaseWide(mode, b)) ||
        (c[0] != L'\0' && EqualsNoCaseWide(mode, c)) || (d[0] != L'\0' && EqualsNoCaseWide(mode, d));
}

void ApplyVoiceChangerModeDefaults(SEffectSettings& settings)
{
    if (!settings.voiceChanger)
    {
        return;
    }

    std::wstring mode = TrimWide(settings.voiceChangerMode);
    if (mode.empty()) mode = L"None";

    double amount = ClampDouble(settings.voiceChangeAmount, 0.0, 100.0) / 100.0;
    if (IsVoiceChangerMode(mode, L"None", L"なし", L"Off", L"無効"))
    {
        return;
    }

    if (IsVoiceChangerMode(mode, L"Male", L"男性", L"男性化", L"男性寄せ"))
    {
        if (settings.voiceChangerEngine.empty() || IsVoiceChangerMode(settings.voiceChangerEngine, L"Builtin", L"標準", L"内蔵")) settings.voiceChangerEngine = L"World";
        if (std::fabs(settings.pitchShiftSemitone) < 0.001) settings.pitchShiftSemitone = -7.0;
        if (std::fabs(settings.formantShiftRatio - 1.0) < 0.001) settings.formantShiftRatio = 0.72;
        if (std::fabs(settings.tempoRatio - 1.0) < 0.001) settings.tempoRatio = 0.96;
        settings.eqLowGainDb = ClampDouble(settings.eqLowGainDb + 4.0 * amount, -24.0, 24.0);
        settings.eqHighGainDb = ClampDouble(settings.eqHighGainDb - 2.5 * amount, -24.0, 24.0);
        settings.compressor = true;
        if (settings.compressorRatio < 2.0) settings.compressorRatio = 2.0;
        return;
    }

    if (IsVoiceChangerMode(mode, L"Female", L"女性", L"女性化", L"女性寄せ"))
    {
        if (settings.voiceChangerEngine.empty() || IsVoiceChangerMode(settings.voiceChangerEngine, L"Builtin", L"標準", L"内蔵")) settings.voiceChangerEngine = L"World";
        if (std::fabs(settings.pitchShiftSemitone) < 0.001) settings.pitchShiftSemitone = 5.5;
        if (std::fabs(settings.formantShiftRatio - 1.0) < 0.001) settings.formantShiftRatio = 1.28;
        if (std::fabs(settings.tempoRatio - 1.0) < 0.001) settings.tempoRatio = 1.03;
        settings.eqLowGainDb = ClampDouble(settings.eqLowGainDb - 2.0 * amount, -24.0, 24.0);
        settings.eqHighGainDb = ClampDouble(settings.eqHighGainDb + 3.0 * amount, -24.0, 24.0);
        return;
    }

    if (IsVoiceChangerMode(mode, L"Deep", L"低音", L"重低音", L"Monster"))
    {
        if (settings.voiceChangerEngine.empty() || IsVoiceChangerMode(settings.voiceChangerEngine, L"Builtin", L"標準", L"内蔵")) settings.voiceChangerEngine = L"World";
        if (std::fabs(settings.pitchShiftSemitone) < 0.001) settings.pitchShiftSemitone = -10.0;
        if (std::fabs(settings.formantShiftRatio - 1.0) < 0.001) settings.formantShiftRatio = 0.62;
        if (std::fabs(settings.tempoRatio - 1.0) < 0.001) settings.tempoRatio = 0.93;
        settings.eqLowGainDb = ClampDouble(settings.eqLowGainDb + 6.0 * amount, -24.0, 24.0);
        settings.eqHighGainDb = ClampDouble(settings.eqHighGainDb - 4.0 * amount, -24.0, 24.0);
        settings.compressor = true;
        if (settings.compressorRatio < 3.0) settings.compressorRatio = 3.0;
        return;
    }

    if (IsVoiceChangerMode(mode, L"Robot", L"ロボット", L"機械", L"機械風"))
    {
        if (settings.voiceChangerEngine.empty() || IsVoiceChangerMode(settings.voiceChangerEngine, L"Builtin", L"標準", L"内蔵")) settings.voiceChangerEngine = L"World";
        if (std::fabs(settings.pitchShiftSemitone) < 0.001) settings.pitchShiftSemitone = -1.0;
        if (std::fabs(settings.formantShiftRatio - 1.0) < 0.001) settings.formantShiftRatio = 0.96;
        if (settings.robotPitchFlatten < 1.0) settings.robotPitchFlatten = 90.0;
        if (settings.robotAmount < 1.0) settings.robotAmount = 35.0;
        if (settings.ringModHz < 1.0) settings.ringModHz = 32.0;
        settings.compressor = true;
        if (settings.compressorRatio < 3.0) settings.compressorRatio = 3.0;
        return;
    }

    if (IsVoiceChangerMode(mode, L"Child", L"子供", L"子ども", L"高音"))
    {
        if (settings.voiceChangerEngine.empty() || IsVoiceChangerMode(settings.voiceChangerEngine, L"Builtin", L"標準", L"内蔵")) settings.voiceChangerEngine = L"World";
        if (std::fabs(settings.pitchShiftSemitone) < 0.001) settings.pitchShiftSemitone = 8.0;
        if (std::fabs(settings.formantShiftRatio - 1.0) < 0.001) settings.formantShiftRatio = 1.38;
        if (std::fabs(settings.tempoRatio - 1.0) < 0.001) settings.tempoRatio = 1.05;
        settings.eqLowGainDb = ClampDouble(settings.eqLowGainDb - 3.0 * amount, -24.0, 24.0);
        settings.eqHighGainDb = ClampDouble(settings.eqHighGainDb + 4.0 * amount, -24.0, 24.0);
        return;
    }
}

std::vector<double> SamplesToDoubleBuffer(const std::vector<short>& samples)
{
    std::vector<double> out(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
    {
        out[i] = SampleToDouble(samples[i]);
    }
    return out;
}

std::vector<short> DoubleBufferToSamples(const std::vector<double>& samples)
{
    std::vector<short> out(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
    {
        out[i] = DoubleToSample(samples[i]);
    }
    return out;
}

size_t FrameCountFromBuffer(const std::vector<double>& data, int channels)
{
    if (channels <= 0) return 0;
    return data.size() / static_cast<size_t>(channels);
}

std::vector<double> ResampleFrames(const std::vector<double>& input, int channels, size_t outputFrames)
{
    std::vector<double> out;
    if (channels <= 0 || input.empty() || outputFrames == 0)
    {
        return out;
    }

    size_t inputFrames = FrameCountFromBuffer(input, channels);
    if (inputFrames == 0)
    {
        return out;
    }
    if (inputFrames == outputFrames)
    {
        return input;
    }

    out.resize(outputFrames * static_cast<size_t>(channels), 0.0);
    if (outputFrames == 1 || inputFrames == 1)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            out[static_cast<size_t>(ch)] = input[static_cast<size_t>(ch)];
        }
        return out;
    }

    double scale = static_cast<double>(inputFrames - 1) / static_cast<double>(outputFrames - 1);
    for (size_t frame = 0; frame < outputFrames; ++frame)
    {
        double srcPos = static_cast<double>(frame) * scale;
        size_t i0 = static_cast<size_t>(srcPos);
        size_t i1 = (std::min)(i0 + 1, inputFrames - 1);
        double frac = srcPos - static_cast<double>(i0);
        for (int ch = 0; ch < channels; ++ch)
        {
            double a = input[i0 * static_cast<size_t>(channels) + ch];
            double b = input[i1 * static_cast<size_t>(channels) + ch];
            out[frame * static_cast<size_t>(channels) + ch] = a + (b - a) * frac;
        }
    }
    return out;
}

double HannValue(size_t index, size_t count)
{
    if (count <= 1) return 1.0;
    return 0.5 - 0.5 * std::cos((2.0 * PI_VALUE * static_cast<double>(index)) / static_cast<double>(count - 1));
}

int ChooseOlaWindow(int sampleRate, size_t frameCount)
{
    int window = static_cast<int>((static_cast<double>((std::max)(sampleRate, 8000)) * 0.045) + 0.5);
    window = ClampInt(window, 256, 4096);
    if ((window & 1) != 0) ++window;
    if (frameCount > 0 && static_cast<size_t>(window) > frameCount)
    {
        window = static_cast<int>(frameCount);
        if (window < 32) window = 32;
        if ((window & 1) != 0) --window;
    }
    return (std::max)(32, window);
}

std::vector<double> TimeStretchOla(const std::vector<double>& input, int channels, int sampleRate, double stretchRatio)
{
    if (channels <= 0 || input.empty()) return input;

    size_t inputFrames = FrameCountFromBuffer(input, channels);
    if (inputFrames < 64) return input;

    stretchRatio = ClampDouble(stretchRatio, 0.35, 3.0);
    if (std::fabs(stretchRatio - 1.0) < 0.005) return input;

    int window = ChooseOlaWindow(sampleRate, inputFrames);
    int outHop = (std::max)(16, window / 4);
    double inHop = static_cast<double>(outHop) / stretchRatio;
    size_t targetFrames = (std::max<size_t>)(1, static_cast<size_t>(std::lround(static_cast<double>(inputFrames) * stretchRatio)));
    size_t estimatedFrames = targetFrames + static_cast<size_t>(window * 2 + outHop * 2);

    std::vector<double> out(estimatedFrames * static_cast<size_t>(channels), 0.0);
    std::vector<double> weight(estimatedFrames, 0.0);

    for (size_t segment = 0; ; ++segment)
    {
        size_t inPos = static_cast<size_t>(std::lround(static_cast<double>(segment) * inHop));
        size_t outPos = static_cast<size_t>(segment * static_cast<size_t>(outHop));
        if (inPos >= inputFrames || outPos >= estimatedFrames)
        {
            break;
        }

        size_t available = (std::min<size_t>)(static_cast<size_t>(window), inputFrames - inPos);
        for (size_t n = 0; n < available; ++n)
        {
            size_t target = outPos + n;
            if (target >= estimatedFrames) break;
            double w = HannValue(n, static_cast<size_t>(window));
            weight[target] += w;
            for (int ch = 0; ch < channels; ++ch)
            {
                out[target * static_cast<size_t>(channels) + ch] += input[(inPos + n) * static_cast<size_t>(channels) + ch] * w;
            }
        }
    }

    std::vector<double> trimmed(targetFrames * static_cast<size_t>(channels), 0.0);
    for (size_t frame = 0; frame < targetFrames; ++frame)
    {
        double w = (frame < weight.size()) ? weight[frame] : 0.0;
        if (w < 0.000001) w = 1.0;
        for (int ch = 0; ch < channels; ++ch)
        {
            if (frame < estimatedFrames)
            {
                trimmed[frame * static_cast<size_t>(channels) + ch] = out[frame * static_cast<size_t>(channels) + ch] / w;
            }
        }
    }
    return trimmed;
}

void Fft(std::vector<SComplexValue>& a, bool inverse)
{
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(a[i], a[j]);
        }
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        double angle = 2.0 * PI_VALUE / static_cast<double>(len);
        if (!inverse) angle = -angle;
        double wlenRe = std::cos(angle);
        double wlenIm = std::sin(angle);
        for (size_t i = 0; i < n; i += len)
        {
            double wRe = 1.0;
            double wIm = 0.0;
            for (size_t j = 0; j < len / 2; ++j)
            {
                SComplexValue u = a[i + j];
                SComplexValue v;
                v.re = a[i + j + len / 2].re * wRe - a[i + j + len / 2].im * wIm;
                v.im = a[i + j + len / 2].re * wIm + a[i + j + len / 2].im * wRe;
                a[i + j].re = u.re + v.re;
                a[i + j].im = u.im + v.im;
                a[i + j + len / 2].re = u.re - v.re;
                a[i + j + len / 2].im = u.im - v.im;

                double nextRe = wRe * wlenRe - wIm * wlenIm;
                double nextIm = wRe * wlenIm + wIm * wlenRe;
                wRe = nextRe;
                wIm = nextIm;
            }
        }
    }

    if (inverse)
    {
        double scale = 1.0 / static_cast<double>(n);
        for (size_t i = 0; i < n; ++i)
        {
            a[i].re *= scale;
            a[i].im *= scale;
        }
    }
}

int ChooseFftWindow(int sampleRate)
{
    if (sampleRate >= 44100) return 4096;
    if (sampleRate >= 22050) return 2048;
    return 1024;
}

double InterpolateVector(const std::vector<double>& values, double pos)
{
    if (values.empty()) return 0.0;
    if (pos <= 0.0) return values.front();
    double maxPos = static_cast<double>(values.size() - 1);
    if (pos >= maxPos) return values.back();

    size_t i0 = static_cast<size_t>(pos);
    size_t i1 = (std::min)(i0 + 1, values.size() - 1);
    double frac = pos - static_cast<double>(i0);
    return values[i0] + (values[i1] - values[i0]) * frac;
}

void ApplySpectralEnvelopeShift(std::vector<double>& data, int channels, int sampleRate, double ratio, double amount)
{
    if (channels <= 0 || data.empty()) return;
    ratio = ClampDouble(ratio, 0.50, 1.80);
    amount = ClampDouble(amount, 0.0, 1.0);
    if (std::fabs(ratio - 1.0) < 0.01 || amount < 0.01) return;

    size_t frames = FrameCountFromBuffer(data, channels);
    int fftSize = ChooseFftWindow(sampleRate);
    if (frames < static_cast<size_t>(fftSize / 2)) return;
    int hop = fftSize / 4;
    int bins = fftSize / 2;
    int smoothBins = ClampInt(static_cast<int>((static_cast<double>(fftSize) * 180.0) / static_cast<double>((std::max)(sampleRate, 8000))), 4, 64);

    std::vector<double> window(static_cast<size_t>(fftSize), 0.0);
    for (int i = 0; i < fftSize; ++i)
    {
        window[static_cast<size_t>(i)] = HannValue(static_cast<size_t>(i), static_cast<size_t>(fftSize));
    }

    for (int ch = 0; ch < channels; ++ch)
    {
        std::vector<double> out((frames + static_cast<size_t>(fftSize)) * static_cast<size_t>(channels), 0.0);
        std::vector<double> weight(frames + static_cast<size_t>(fftSize), 0.0);

        for (size_t pos = 0; pos < frames; pos += static_cast<size_t>(hop))
        {
            std::vector<SComplexValue> spec(static_cast<size_t>(fftSize));
            for (int n = 0; n < fftSize; ++n)
            {
                size_t frame = pos + static_cast<size_t>(n);
                double x = 0.0;
                if (frame < frames)
                {
                    x = data[frame * static_cast<size_t>(channels) + ch];
                }
                spec[static_cast<size_t>(n)].re = x * window[static_cast<size_t>(n)];
                spec[static_cast<size_t>(n)].im = 0.0;
            }

            Fft(spec, false);

            std::vector<double> logMag(static_cast<size_t>(bins + 1), 0.0);
            for (int k = 0; k <= bins; ++k)
            {
                double re = spec[static_cast<size_t>(k)].re;
                double im = spec[static_cast<size_t>(k)].im;
                logMag[static_cast<size_t>(k)] = std::log(std::sqrt(re * re + im * im) + 1.0e-9);
            }

            std::vector<double> prefix(logMag.size() + 1, 0.0);
            for (size_t k = 0; k < logMag.size(); ++k)
            {
                prefix[k + 1] = prefix[k] + logMag[k];
            }

            std::vector<double> envelope(logMag.size(), 0.0);
            for (int k = 0; k <= bins; ++k)
            {
                int lo = (std::max)(0, k - smoothBins);
                int hi = (std::min)(bins, k + smoothBins);
                double sum = prefix[static_cast<size_t>(hi + 1)] - prefix[static_cast<size_t>(lo)];
                envelope[static_cast<size_t>(k)] = sum / static_cast<double>(hi - lo + 1);
            }

            for (int k = 1; k <= bins; ++k)
            {
                double hz = (static_cast<double>(k) * static_cast<double>(sampleRate)) / static_cast<double>(fftSize);
                if (hz < 70.0 || hz > static_cast<double>(sampleRate) * 0.45)
                {
                    continue;
                }

                double shifted = InterpolateVector(envelope, static_cast<double>(k) / ratio);
                double gainLog = ClampDouble((shifted - envelope[static_cast<size_t>(k)]) * amount, -2.2, 2.2);
                double gain = std::exp(gainLog);
                spec[static_cast<size_t>(k)].re *= gain;
                spec[static_cast<size_t>(k)].im *= gain;
                if (k < bins)
                {
                    size_t mirror = static_cast<size_t>(fftSize - k);
                    spec[mirror].re *= gain;
                    spec[mirror].im *= gain;
                }
            }

            Fft(spec, true);

            for (int n = 0; n < fftSize; ++n)
            {
                size_t frame = pos + static_cast<size_t>(n);
                if (frame >= weight.size()) break;
                double w = window[static_cast<size_t>(n)];
                out[frame * static_cast<size_t>(channels) + ch] += spec[static_cast<size_t>(n)].re * w;
                weight[frame] += w * w;
            }
        }

        for (size_t frame = 0; frame < frames; ++frame)
        {
            double w = weight[frame];
            if (w < 1.0e-9) w = 1.0;
            data[frame * static_cast<size_t>(channels) + ch] = out[frame * static_cast<size_t>(channels) + ch] / w;
        }
    }
}

void ApplyRobotModulation(std::vector<double>& data, int channels, int sampleRate, double amount, double ringHz)
{
    if (channels <= 0 || data.empty()) return;
    amount = ClampDouble(amount, 0.0, 100.0) / 100.0;
    if (amount < 0.01) return;

    ringHz = ClampDouble(ringHz, 5.0, 180.0);
    size_t frames = FrameCountFromBuffer(data, channels);
    double phase = 0.0;
    double phaseStep = 2.0 * PI_VALUE * ringHz / static_cast<double>((std::max)(sampleRate, 8000));
    // 量子化を強くしすぎると「ガビガビ」になりやすいため控えめにする。
    double quantMix = amount * 0.10;

    for (size_t frame = 0; frame < frames; ++frame)
    {
        double carrier = std::sin(phase);
        phase += phaseStep;
        if (phase > 2.0 * PI_VALUE) phase -= 2.0 * PI_VALUE;

        for (int ch = 0; ch < channels; ++ch)
        {
            size_t i = frame * static_cast<size_t>(channels) + ch;
            double original = data[i];
            double ringed = original * carrier;
            double y = original * (1.0 - amount) + ringed * amount;
            double quantized = std::floor(y * 24.0 + (y >= 0.0 ? 0.5 : -0.5)) / 24.0;
            data[i] = y * (1.0 - quantMix) + quantized * quantMix;
        }
    }
}

void MixDryWet(std::vector<double>& processed, const std::vector<double>& original, int channels, double dryWet)
{
    if (channels <= 0 || processed.empty() || original.empty()) return;
    double wet = ClampDouble(dryWet, 0.0, 100.0) / 100.0;
    if (wet >= 0.999) return;

    size_t frames = FrameCountFromBuffer(processed, channels);
    std::vector<double> dry = ResampleFrames(original, channels, frames);
    if (dry.size() != processed.size()) return;

    for (size_t i = 0; i < processed.size(); ++i)
    {
        processed[i] = dry[i] * (1.0 - wet) + processed[i] * wet;
    }
}


bool ShouldUseWorldVoiceChanger(const SEffectSettings& settings)
{
    if (!settings.voiceChanger) return false;
    if (IsVoiceChangerMode(settings.voiceChangerEngine, L"World", L"WORLD", L"ワールド", L"WORLDボコーダー")) return true;
    if (IsVoiceChangerMode(settings.voiceChangerEngine, L"Auto", L"自動", L"優先", L"World優先")) return true;
    return false;
}

double MedianVoicedF0(const std::vector<double>& f0)
{
    std::vector<double> voiced;
    voiced.reserve(f0.size());
    for (size_t i = 0; i < f0.size(); ++i)
    {
        if (f0[i] > 10.0)
        {
            voiced.push_back(f0[i]);
        }
    }
    if (voiced.empty()) return 0.0;
    size_t mid = voiced.size() / 2;
    std::nth_element(voiced.begin(), voiced.begin() + mid, voiced.end());
    return voiced[mid];
}

#if WVS_HAS_WORLD
struct SWorldMatrix
{
    int rows = 0;
    int cols = 0;
    std::vector<double> values;
    std::vector<double*> ptrs;

    bool Allocate(int rowCount, int colCount)
    {
        rows = rowCount;
        cols = colCount;
        if (rows <= 0 || cols <= 0) return false;
        values.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0.0);
        ptrs.resize(static_cast<size_t>(rows), nullptr);
        for (int r = 0; r < rows; ++r)
        {
            ptrs[static_cast<size_t>(r)] = values.data() + static_cast<size_t>(r) * static_cast<size_t>(cols);
        }
        return true;
    }
};

void ShiftWorldSpectralEnvelope(SWorldMatrix& spectrogram, double ratio, double amount)
{
    ratio = ClampDouble(ratio, 0.50, 1.80);
    amount = ClampDouble(amount, 0.0, 1.0);
    if (std::fabs(ratio - 1.0) < 0.01 || amount < 0.01 || spectrogram.rows <= 0 || spectrogram.cols <= 2)
    {
        return;
    }

    std::vector<double> src(static_cast<size_t>(spectrogram.cols), 0.0);
    for (int t = 0; t < spectrogram.rows; ++t)
    {
        double* row = spectrogram.ptrs[static_cast<size_t>(t)];
        for (int k = 0; k < spectrogram.cols; ++k)
        {
            src[static_cast<size_t>(k)] = std::log((std::max)(row[k], 1.0e-12));
        }

        for (int k = 1; k < spectrogram.cols; ++k)
        {
            double pos = static_cast<double>(k) / ratio;
            double shifted = InterpolateVector(src, pos);
            double mixedLog = src[static_cast<size_t>(k)] * (1.0 - amount) + shifted * amount;
            row[k] = (std::max)(1.0e-12, std::exp(ClampDouble(mixedLog, -60.0, 20.0)));
        }
    }
}

void ScaleWorldAperiodicity(SWorldMatrix& aperiodicity, double scale, double amount)
{
    scale = ClampDouble(scale, 0.20, 3.00);
    amount = ClampDouble(amount, 0.0, 1.0);
    if (std::fabs(scale - 1.0) < 0.01 || amount < 0.01) return;

    for (size_t i = 0; i < aperiodicity.values.size(); ++i)
    {
        double current = ClampDouble(aperiodicity.values[i], 0.0, 1.0);
        double scaled = ClampDouble(current * scale, 0.0, 1.0);
        aperiodicity.values[i] = current * (1.0 - amount) + scaled * amount;
    }
}

bool ApplyWorldVoiceChangerChannel(
    const std::vector<double>& input,
    std::vector<double>& output,
    int sampleRate,
    const SEffectSettings& settings,
    double amount)
{
    output = input;
    if (input.size() < 256 || sampleRate < 8000) return false;

    const int xLength = static_cast<int>((std::min<size_t>)(input.size(), static_cast<size_t>(INT_MAX)));
    const double framePeriod = ClampDouble(settings.worldFramePeriodMs, 1.0, 10.0);
    const double f0Floor = ClampDouble(settings.worldF0FloorHz, 40.0, 300.0);
    const double f0Ceil = ClampDouble(settings.worldF0CeilHz, (std::max)(f0Floor + 50.0, 120.0), 1200.0);

    ::DioOption dioOption;
    ::InitializeDioOption(&dioOption);
    dioOption.frame_period = framePeriod;
    dioOption.f0_floor = f0Floor;
    dioOption.f0_ceil = f0Ceil;

    const int f0Length = ::GetSamplesForDIO(sampleRate, xLength, framePeriod);
    if (f0Length <= 0) return false;

    std::vector<double> temporalPositions(static_cast<size_t>(f0Length), 0.0);
    std::vector<double> f0(static_cast<size_t>(f0Length), 0.0);
    std::vector<double> refinedF0(static_cast<size_t>(f0Length), 0.0);

    ::Dio(input.data(), xLength, sampleRate, &dioOption, temporalPositions.data(), f0.data());
    ::StoneMask(input.data(), xLength, sampleRate, temporalPositions.data(), f0.data(), f0Length, refinedF0.data());

    ::CheapTrickOption cheapTrickOption;
    ::InitializeCheapTrickOption(sampleRate, &cheapTrickOption);
    cheapTrickOption.f0_floor = f0Floor;
    const int fftSize = ::GetFFTSizeForCheapTrick(sampleRate, &cheapTrickOption);
    const int bins = fftSize / 2 + 1;
    if (fftSize <= 0 || bins <= 1) return false;

    SWorldMatrix spectrogram;
    SWorldMatrix aperiodicity;
    if (!spectrogram.Allocate(f0Length, bins) || !aperiodicity.Allocate(f0Length, bins)) return false;

    ::CheapTrick(input.data(), xLength, sampleRate, temporalPositions.data(), refinedF0.data(), f0Length, &cheapTrickOption, spectrogram.ptrs.data());

    ::D4COption d4cOption;
    ::InitializeD4COption(&d4cOption);
    ::D4C(input.data(), xLength, sampleRate, temporalPositions.data(), refinedF0.data(), f0Length, fftSize, &d4cOption, aperiodicity.ptrs.data());

    double pitchSemitone = ClampDouble(settings.pitchShiftSemitone, -24.0, 24.0) * amount;
    double pitchScale = std::pow(2.0, pitchSemitone / 12.0);
    pitchScale *= 1.0 + (ClampDouble(settings.f0Scale, 0.25, 4.0) - 1.0) * amount;
    pitchScale = ClampDouble(pitchScale, 0.25, 4.0);

    double flatten = ClampDouble(settings.robotPitchFlatten, 0.0, 100.0) / 100.0 * amount;
    double centerF0 = MedianVoicedF0(refinedF0);
    for (size_t i = 0; i < refinedF0.size(); ++i)
    {
        if (refinedF0[i] <= 10.0) continue;
        double voicedF0 = refinedF0[i];
        if (centerF0 > 10.0 && flatten > 0.001)
        {
            voicedF0 = voicedF0 * (1.0 - flatten) + centerF0 * flatten;
        }
        refinedF0[i] = ClampDouble(voicedF0 * pitchScale, f0Floor, f0Ceil);
    }

    double formantRatio = 1.0 + (ClampDouble(settings.formantShiftRatio, 0.50, 1.80) - 1.0) * amount;
    ShiftWorldSpectralEnvelope(spectrogram, formantRatio, amount);

    double apScale = 1.0 + (ClampDouble(settings.aperiodicityScale, 0.20, 3.00) - 1.0) * amount;
    ScaleWorldAperiodicity(aperiodicity, apScale, amount);

    std::vector<double> synthesized(input.size(), 0.0);
    ::Synthesis(refinedF0.data(), f0Length, spectrogram.ptrs.data(), aperiodicity.ptrs.data(), fftSize, framePeriod, sampleRate, xLength, synthesized.data());
    output.swap(synthesized);
    return true;
}
#endif

bool TryApplyWorldVoiceChanger(std::vector<double>& data, int channels, int sampleRate, const SEffectSettings& settings, double amount)
{
    if (!ShouldUseWorldVoiceChanger(settings)) return false;
#if WVS_HAS_WORLD
    if (channels <= 0 || data.empty()) return false;
    size_t frames = FrameCountFromBuffer(data, channels);
    if (frames < 256) return false;

    std::vector<double> out(data.size(), 0.0);
    bool anySuccess = false;
    for (int ch = 0; ch < channels; ++ch)
    {
        std::vector<double> input(frames, 0.0);
        for (size_t frame = 0; frame < frames; ++frame)
        {
            input[frame] = data[frame * static_cast<size_t>(channels) + ch];
        }

        std::vector<double> converted;
        if (!ApplyWorldVoiceChangerChannel(input, converted, sampleRate, settings, amount) || converted.size() != frames)
        {
            return false;
        }

        for (size_t frame = 0; frame < frames; ++frame)
        {
            out[frame * static_cast<size_t>(channels) + ch] = converted[frame];
        }
        anySuccess = true;
    }

    if (anySuccess)
    {
        data.swap(out);
        return true;
    }
    return false;
#else
    (void)data;
    (void)channels;
    (void)sampleRate;
    (void)settings;
    (void)amount;
    return false;
#endif
}

void ApplyVoiceChanger(SWavData& wav, const SEffectSettings& settings)
{
    if (!settings.voiceChanger || wav.samples.empty() || wav.format.channels == 0)
    {
        return;
    }

    int channels = static_cast<int>(wav.format.channels);
    int sampleRate = static_cast<int>((std::max<DWORD>)(wav.format.sampleRate, 8000));
    std::vector<double> original = SamplesToDoubleBuffer(wav.samples);
    std::vector<double> processed = original;

    double amount = ClampDouble(settings.voiceChangeAmount, 0.0, 100.0) / 100.0;
    if (amount <= 0.001)
    {
        return;
    }

    double tempoRatio = 1.0 + (ClampDouble(settings.tempoRatio, 0.50, 1.80) - 1.0) * amount;
    if (std::fabs(tempoRatio - 1.0) > 0.01)
    {
        processed = TimeStretchOla(processed, channels, sampleRate, 1.0 / tempoRatio);
    }

    bool worldApplied = TryApplyWorldVoiceChanger(processed, channels, sampleRate, settings, amount);
    if (!worldApplied)
    {
        double pitchSemitone = ClampDouble(settings.pitchShiftSemitone, -24.0, 24.0) * amount;
        if (std::fabs(pitchSemitone) > 0.05)
        {
            size_t targetFrames = FrameCountFromBuffer(processed, channels);
            double pitchFactor = std::pow(2.0, pitchSemitone / 12.0);
            std::vector<double> stretched = TimeStretchOla(processed, channels, sampleRate, pitchFactor);
            processed = ResampleFrames(stretched, channels, targetFrames);
        }

        double formantRatio = 1.0 + (ClampDouble(settings.formantShiftRatio, 0.50, 1.80) - 1.0) * amount;
        ApplySpectralEnvelopeShift(processed, channels, sampleRate, formantRatio, amount);
    }

    ApplyRobotModulation(processed, channels, sampleRate, settings.robotAmount * amount, settings.ringModHz);
    MixDryWet(processed, original, channels, settings.dryWet);

    wav.samples = DoubleBufferToSamples(processed);
}

void ProcessWav(SWavData& wav, const SEffectSettings& settings)
{
    SEffectSettings effectiveSettings = settings;
    ApplyVoiceChangerModeDefaults(effectiveSettings);

    TrimSilence(wav, effectiveSettings);
    RemoveDcOffset(wav, effectiveSettings);
    ApplyGain(wav, effectiveSettings.preGainDb);
    ApplyVoiceChanger(wav, effectiveSettings);
    ApplyNoiseGate(wav, effectiveSettings);
    ApplyEq(wav, effectiveSettings);
    ApplyCompressor(wav, effectiveSettings);
    ApplyNormalize(wav, effectiveSettings);
    ApplyGain(wav, effectiveSettings.outputGainDb);
    ApplyLimiter(wav, effectiveSettings);
    ApplyFade(wav, effectiveSettings);
}

std::wstring DecodeUtf8Text(const unsigned char* data, int len)
{
    if (len <= 0) return std::wstring();

    int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(data), len, nullptr, 0);
    if (wlen <= 0)
    {
        wlen = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCCH>(data), len, nullptr, 0);
    }
    if (wlen <= 0) return std::wstring();

    std::wstring out;
    out.resize(static_cast<size_t>(wlen));
    ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCCH>(data), len, &out[0], wlen);
    return out;
}

std::wstring DecodeCp932Text(const unsigned char* data, int len)
{
    if (len <= 0) return std::wstring();

    int wlen = ::MultiByteToWideChar(932, 0, reinterpret_cast<LPCCH>(data), len, nullptr, 0);
    if (wlen <= 0) return std::wstring();

    std::wstring out;
    out.resize(static_cast<size_t>(wlen));
    ::MultiByteToWideChar(932, 0, reinterpret_cast<LPCCH>(data), len, &out[0], wlen);
    return out;
}

std::wstring ReadTextFileAutoText(const std::wstring& path)
{
    std::vector<unsigned char> bytes;
    if (!ReadAllBytes(path, bytes) || bytes.empty())
    {
        return std::wstring();
    }

    const unsigned char* p = bytes.data();
    int len = static_cast<int>(bytes.size());

    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
    {
        return DecodeUtf8Text(p + 3, len - 3);
    }
    if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE)
    {
        return std::wstring(reinterpret_cast<const wchar_t*>(p + 2), static_cast<size_t>((len - 2) / 2));
    }
    if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF)
    {
        int chars = (len - 2) / 2;
        std::wstring out;
        out.resize(static_cast<size_t>(chars));
        for (int i = 0; i < chars; ++i)
        {
            out[static_cast<size_t>(i)] = static_cast<wchar_t>((p[2 + i * 2] << 8) | p[3 + i * 2]);
        }
        return out;
    }

    std::wstring utf8 = DecodeUtf8Text(p, len);
    if (!utf8.empty()) return utf8;
    return DecodeCp932Text(p, len);
}

std::wstring TrimWide(std::wstring value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first]))
    {
        ++first;
    }

    size_t last = value.size();
    while (last > first && iswspace(value[last - 1]))
    {
        --last;
    }

    return value.substr(first, last - first);
}

bool EqualsNoCaseWide(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool TryGetIniValueFromText(const std::wstring& text, const std::wstring& section, const std::wstring& key, std::wstring& value)
{
    std::wstring currentSection;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(L'\n', start);
        std::wstring line = (end == std::wstring::npos)
            ? text.substr(start)
            : text.substr(start, end - start);

        if (end == std::wstring::npos)
        {
            start = text.size() + 1;
        }
        else
        {
            start = end + 1;
        }

        line = TrimWide(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#')
        {
            continue;
        }

        if (line.size() >= 3 && line[0] == L'[')
        {
            size_t close = line.find(L']');
            if (close != std::wstring::npos && close > 1)
            {
                currentSection = TrimWide(line.substr(1, close - 1));
            }
            continue;
        }

        if (!EqualsNoCaseWide(currentSection, section))
        {
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
        {
            continue;
        }

        std::wstring name = TrimWide(line.substr(0, eq));
        if (!EqualsNoCaseWide(name, key))
        {
            continue;
        }

        value = TrimWide(line.substr(eq + 1));
        if (value.size() >= 2 &&
            ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\'')))
        {
            value = value.substr(1, value.size() - 2);
        }
        return true;
    }
    return false;
}

int ReadIniIntSafe(const std::wstring& iniText, const std::wstring& section, const std::wstring& key, int fallback)
{
    std::wstring text;
    if (!TryGetIniValueFromText(iniText, section, key, text))
    {
        return fallback;
    }

    wchar_t* end = nullptr;
    long value = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return fallback;
    return static_cast<int>(value);
}

double ReadIniDoubleSafe(const std::wstring& iniText, const std::wstring& section, const std::wstring& key, double fallback)
{
    std::wstring text;
    if (!TryGetIniValueFromText(iniText, section, key, text))
    {
        return fallback;
    }

    wchar_t* end = nullptr;
    double value = wcstod(text.c_str(), &end);
    if (end == text.c_str()) return fallback;
    return value;
}

std::wstring ReadIniStringSafe(const std::wstring& iniText, const std::wstring& section, const std::wstring& key, const std::wstring& fallback)
{
    std::wstring text;
    if (!TryGetIniValueFromText(iniText, section, key, text))
    {
        return fallback;
    }
    return text;
}

bool ParseDoubleStrict(const std::wstring& text, double& value)
{
    std::wstring t = TrimWide(text);
    if (t.empty()) return false;

    wchar_t* end = nullptr;
    double parsed = wcstod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    while (end != nullptr && *end != 0)
    {
        if (!iswspace(*end)) return false;
        ++end;
    }
    value = parsed;
    return true;
}

bool ParseIntStrict(const std::wstring& text, int& value)
{
    std::wstring t = TrimWide(text);
    if (t.empty()) return false;

    wchar_t* end = nullptr;
    long parsed = wcstol(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    while (end != nullptr && *end != 0)
    {
        if (!iswspace(*end)) return false;
        ++end;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseBoolStrict(const std::wstring& text, bool& value)
{
    std::wstring t = TrimWide(text);
    if (EqualsNoCaseWide(t, L"true") || EqualsNoCaseWide(t, L"on") || EqualsNoCaseWide(t, L"yes"))
    {
        value = true;
        return true;
    }
    if (EqualsNoCaseWide(t, L"false") || EqualsNoCaseWide(t, L"off") || EqualsNoCaseWide(t, L"no"))
    {
        value = false;
        return true;
    }

    int i = 0;
    if (!ParseIntStrict(t, i)) return false;
    value = (i != 0);
    return true;
}

bool ApplyEffectSettingOverride(SEffectSettings& s, const std::wstring& key, const std::wstring& value)
{
    double d = 0.0;
    int i = 0;
    bool b = false;

    if (EqualsNoCaseWide(key, L"Normalize"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.normalize = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"TargetPeakDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.targetPeakDb = ClampDouble(d, -60.0, 0.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"PreGainDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.preGainDb = ClampDouble(d, -36.0, 36.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"OutputGainDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.outputGainDb = ClampDouble(d, -36.0, 36.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"RemoveDcOffset"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.removeDcOffset = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"TrimSilence"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.trimSilence = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"TrimThresholdDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.trimThresholdDb = ClampDouble(d, -90.0, -5.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"TrimPaddingMs"))
    {
        if (!ParseIntStrict(value, i)) return false;
        s.trimPaddingMs = ClampInt(i, 0, 5000);
        return true;
    }
    if (EqualsNoCaseWide(key, L"FadeInMs"))
    {
        if (!ParseIntStrict(value, i)) return false;
        s.fadeInMs = ClampInt(i, 0, 10000);
        return true;
    }
    if (EqualsNoCaseWide(key, L"FadeOutMs"))
    {
        if (!ParseIntStrict(value, i)) return false;
        s.fadeOutMs = ClampInt(i, 0, 10000);
        return true;
    }
    if (EqualsNoCaseWide(key, L"Limiter"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.limiter = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"LimiterThresholdDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.limiterThresholdDb = ClampDouble(d, -60.0, 0.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"Compressor"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.compressor = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"CompressorThresholdDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.compressorThresholdDb = ClampDouble(d, -80.0, 0.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"CompressorRatio"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.compressorRatio = ClampDouble(d, 1.0, 20.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"EqLowGainDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.eqLowGainDb = ClampDouble(d, -24.0, 24.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"EqMidGainDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.eqMidGainDb = ClampDouble(d, -24.0, 24.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"EqHighGainDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.eqHighGainDb = ClampDouble(d, -24.0, 24.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"EqLowCutHz"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.eqLowCutHz = ClampDouble(d, 20.0, 8000.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"EqHighCutHz"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.eqHighCutHz = ClampDouble(d, 100.0, 20000.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"NoiseGate"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.noiseGate = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"NoiseGateThresholdDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.noiseGateThresholdDb = ClampDouble(d, -100.0, -1.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"NoiseGateFloorDb"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.noiseGateFloorDb = ClampDouble(d, -120.0, 0.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"VoiceChanger") || EqualsNoCaseWide(key, L"VoiceChangerEnable"))
    {
        if (!ParseBoolStrict(value, b)) return false;
        s.voiceChanger = b;
        return true;
    }
    if (EqualsNoCaseWide(key, L"VoiceChangerMode"))
    {
        s.voiceChangerMode = TrimWide(value);
        if (s.voiceChangerMode.empty()) s.voiceChangerMode = L"None";
        return true;
    }
    if (EqualsNoCaseWide(key, L"VoiceChangerEngine"))
    {
        s.voiceChangerEngine = TrimWide(value);
        if (s.voiceChangerEngine.empty()) s.voiceChangerEngine = L"Builtin";
        return true;
    }
    if (EqualsNoCaseWide(key, L"VoiceChangeAmount"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.voiceChangeAmount = ClampDouble(d, 0.0, 100.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"F0Scale"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.f0Scale = ClampDouble(d, 0.25, 4.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"AperiodicityScale"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.aperiodicityScale = ClampDouble(d, 0.20, 3.00);
        return true;
    }
    if (EqualsNoCaseWide(key, L"RobotPitchFlatten"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.robotPitchFlatten = ClampDouble(d, 0.0, 100.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"WorldFramePeriodMs"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.worldFramePeriodMs = ClampDouble(d, 1.0, 10.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"WorldF0FloorHz"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.worldF0FloorHz = ClampDouble(d, 40.0, 300.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"WorldF0CeilHz"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.worldF0CeilHz = ClampDouble(d, 120.0, 1200.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"PitchShiftSemitone"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.pitchShiftSemitone = ClampDouble(d, -24.0, 24.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"FormantShiftRatio"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.formantShiftRatio = ClampDouble(d, 0.50, 1.80);
        return true;
    }
    if (EqualsNoCaseWide(key, L"TempoRatio"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.tempoRatio = ClampDouble(d, 0.50, 1.80);
        return true;
    }
    if (EqualsNoCaseWide(key, L"DryWet"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.dryWet = ClampDouble(d, 0.0, 100.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"RobotAmount"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.robotAmount = ClampDouble(d, 0.0, 100.0);
        return true;
    }
    if (EqualsNoCaseWide(key, L"RingModHz"))
    {
        if (!ParseDoubleStrict(value, d)) return false;
        s.ringModHz = ClampDouble(d, 5.0, 180.0);
        return true;
    }

    return false;
}

void ApplyEffectSettingOverrides(SEffectSettings& s, const std::vector<std::pair<std::wstring, std::wstring> >& overrides)
{
    for (size_t n = 0; n < overrides.size(); ++n)
    {
        ApplyEffectSettingOverride(s, TrimWide(overrides[n].first), TrimWide(overrides[n].second));
    }
}

bool ParseOverrideArgument(const std::wstring& arg, std::pair<std::wstring, std::wstring>& out)
{
    size_t eq = arg.find(L'=');
    if (eq == std::wstring::npos || eq == 0) return false;

    out.first = TrimWide(arg.substr(0, eq));
    out.second = TrimWide(arg.substr(eq + 1));
    return !out.first.empty();
}

SEffectSettings LoadEffectSettings(const std::wstring& configPath, const std::wstring& presetName)
{
    std::wstring iniText = ReadTextFileAutoText(configPath);

    std::wstring preset = presetName.empty()
        ? ReadIniStringSafe(iniText, L"General", L"DefaultPreset", L"標準")
        : presetName;
    preset = TrimWide(preset);
    if (preset.empty()) preset = L"標準";

    std::wstring section = L"Preset." + preset;
    SEffectSettings s;
    s.normalize = ReadIniIntSafe(iniText, section, L"Normalize", 0) != 0;
    s.targetPeakDb = ReadIniDoubleSafe(iniText, section, L"TargetPeakDb", -1.0);
    s.preGainDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"PreGainDb", 0.0), -36.0, 36.0);
    s.outputGainDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"OutputGainDb", 0.0), -36.0, 36.0);
    s.removeDcOffset = ReadIniIntSafe(iniText, section, L"RemoveDcOffset", 0) != 0;

    s.trimSilence = ReadIniIntSafe(iniText, section, L"TrimSilence", 0) != 0;
    s.trimThresholdDb = ReadIniDoubleSafe(iniText, section, L"TrimThresholdDb", -45.0);
    s.trimPaddingMs = ClampInt(ReadIniIntSafe(iniText, section, L"TrimPaddingMs", 80), 0, 1000);

    s.fadeInMs = ClampInt(ReadIniIntSafe(iniText, section, L"FadeInMs", 0), 0, 5000);
    s.fadeOutMs = ClampInt(ReadIniIntSafe(iniText, section, L"FadeOutMs", 0), 0, 5000);

    s.limiter = ReadIniIntSafe(iniText, section, L"Limiter", 0) != 0;
    s.limiterThresholdDb = ReadIniDoubleSafe(iniText, section, L"LimiterThresholdDb", -1.0);

    s.compressor = ReadIniIntSafe(iniText, section, L"Compressor", 0) != 0;
    s.compressorThresholdDb = ReadIniDoubleSafe(iniText, section, L"CompressorThresholdDb", -16.0);
    s.compressorRatio = ReadIniDoubleSafe(iniText, section, L"CompressorRatio", 2.0);

    s.eqLowGainDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"EqLowGainDb", 0.0), -24.0, 24.0);
    s.eqMidGainDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"EqMidGainDb", 0.0), -24.0, 24.0);
    s.eqHighGainDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"EqHighGainDb", 0.0), -24.0, 24.0);
    s.eqLowCutHz = ClampDouble(ReadIniDoubleSafe(iniText, section, L"EqLowCutHz", 250.0), 20.0, 8000.0);
    s.eqHighCutHz = ClampDouble(ReadIniDoubleSafe(iniText, section, L"EqHighCutHz", 3500.0), 100.0, 20000.0);

    s.noiseGate = ReadIniIntSafe(iniText, section, L"NoiseGate", 0) != 0;
    s.noiseGateThresholdDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"NoiseGateThresholdDb", -55.0), -100.0, -1.0);
    s.noiseGateFloorDb = ClampDouble(ReadIniDoubleSafe(iniText, section, L"NoiseGateFloorDb", -80.0), -120.0, 0.0);

    s.voiceChanger = ReadIniIntSafe(iniText, section, L"VoiceChanger", 0) != 0;
    s.voiceChangerMode = ReadIniStringSafe(iniText, section, L"VoiceChangerMode", L"None");
    if (TrimWide(s.voiceChangerMode).empty()) s.voiceChangerMode = L"None";
    s.voiceChangerEngine = ReadIniStringSafe(iniText, section, L"VoiceChangerEngine", L"Builtin");
    if (TrimWide(s.voiceChangerEngine).empty()) s.voiceChangerEngine = L"Builtin";
    s.voiceChangeAmount = ClampDouble(ReadIniDoubleSafe(iniText, section, L"VoiceChangeAmount", 70.0), 0.0, 100.0);
    s.f0Scale = ClampDouble(ReadIniDoubleSafe(iniText, section, L"F0Scale", 1.0), 0.25, 4.0);
    s.aperiodicityScale = ClampDouble(ReadIniDoubleSafe(iniText, section, L"AperiodicityScale", 1.0), 0.20, 3.00);
    s.robotPitchFlatten = ClampDouble(ReadIniDoubleSafe(iniText, section, L"RobotPitchFlatten", 0.0), 0.0, 100.0);
    s.worldFramePeriodMs = ClampDouble(ReadIniDoubleSafe(iniText, section, L"WorldFramePeriodMs", 5.0), 1.0, 10.0);
    s.worldF0FloorHz = ClampDouble(ReadIniDoubleSafe(iniText, section, L"WorldF0FloorHz", 71.0), 40.0, 300.0);
    s.worldF0CeilHz = ClampDouble(ReadIniDoubleSafe(iniText, section, L"WorldF0CeilHz", 800.0), 120.0, 1200.0);
    s.pitchShiftSemitone = ClampDouble(ReadIniDoubleSafe(iniText, section, L"PitchShiftSemitone", 0.0), -24.0, 24.0);
    s.formantShiftRatio = ClampDouble(ReadIniDoubleSafe(iniText, section, L"FormantShiftRatio", 1.0), 0.50, 1.80);
    s.tempoRatio = ClampDouble(ReadIniDoubleSafe(iniText, section, L"TempoRatio", 1.0), 0.50, 1.80);
    s.dryWet = ClampDouble(ReadIniDoubleSafe(iniText, section, L"DryWet", 100.0), 0.0, 100.0);
    s.robotAmount = ClampDouble(ReadIniDoubleSafe(iniText, section, L"RobotAmount", 0.0), 0.0, 100.0);
    s.ringModHz = ClampDouble(ReadIniDoubleSafe(iniText, section, L"RingModHz", 35.0), 5.0, 180.0);
    return s;
}

void PrintHelp()
{
    std::fwprintf(stdout, L"WavVoiceShaper - WAV成形ツール\n");
    std::fwprintf(stdout, L"Usage:\n");
    std::fwprintf(stdout, L"  WavVoiceShaper.exe --in input.wav --out output.wav [--config WavVoiceShaper.ini] [--preset 標準]\n");
    std::fwprintf(stdout, L"  WavVoiceShaper.exe --in input.wav --out output.wav --preset 明瞭化 --set PreGainDb=2.0 --set EqHighGainDb=3.0\n");
    std::fwprintf(stdout, L"\nCustom keys:\n");
    std::fwprintf(stdout, L"  Normalize, TargetPeakDb, PreGainDb, OutputGainDb, RemoveDcOffset, TrimSilence, TrimThresholdDb, TrimPaddingMs\n");
    std::fwprintf(stdout, L"  FadeInMs, FadeOutMs, Limiter, LimiterThresholdDb, Compressor, CompressorThresholdDb, CompressorRatio\n");
    std::fwprintf(stdout, L"  EqLowGainDb, EqMidGainDb, EqHighGainDb, EqLowCutHz, EqHighCutHz, NoiseGate, NoiseGateThresholdDb, NoiseGateFloorDb\n");
    std::fwprintf(stdout, L"  VoiceChanger, VoiceChangerMode, VoiceChangerEngine, VoiceChangeAmount, PitchShiftSemitone, FormantShiftRatio, TempoRatio, DryWet\n");
    std::fwprintf(stdout, L"  F0Scale, AperiodicityScale, RobotPitchFlatten, WorldFramePeriodMs, WorldF0FloorHz, WorldF0CeilHz, RobotAmount, RingModHz\n");
    std::fwprintf(stdout, L"\nExit codes:\n");
    std::fwprintf(stdout, L"  0 success\n  1 argument error\n  2 input not found\n  3 read/format error\n  4 process error\n  5 write error\n");
}

bool ParseArgs(int argc, wchar_t* argv[], SOptions& opt)
{
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--help" || a == L"/?" || a == L"-h")
        {
            opt.showHelp = true;
            return true;
        }
        if ((a == L"--in" || a == L"-i") && i + 1 < argc)
        {
            opt.inPath = argv[++i];
            continue;
        }
        if ((a == L"--out" || a == L"-o") && i + 1 < argc)
        {
            opt.outPath = argv[++i];
            continue;
        }
        if (a == L"--config" && i + 1 < argc)
        {
            opt.configPath = argv[++i];
            continue;
        }
        if (a == L"--preset" && i + 1 < argc)
        {
            opt.presetName = argv[++i];
            continue;
        }
        if ((a == L"--set" || a == L"--override") && i + 1 < argc)
        {
            std::pair<std::wstring, std::wstring> item;
            if (!ParseOverrideArgument(argv[++i], item))
            {
                return false;
            }
            opt.overrides.push_back(item);
            continue;
        }
        return false;
    }

    if (opt.configPath.empty()) opt.configPath = L"WavVoiceShaper.ini";
    if (opt.presetName.empty()) opt.presetName = L"標準";
    return !opt.inPath.empty() && !opt.outPath.empty();
}
} // namespace

int wmain(int argc, wchar_t* argv[])
{
    SOptions opt;
    if (!ParseArgs(argc, argv, opt))
    {
        PrintHelp();
        return EXIT_ARGUMENT_ERROR;
    }
    if (opt.showHelp)
    {
        PrintHelp();
        return EXIT_OK;
    }

    opt.inPath = ResolvePath(opt.inPath);
    opt.outPath = ResolvePath(opt.outPath);
    opt.configPath = ResolvePath(opt.configPath);

    if (!FileExists(opt.inPath))
    {
        std::fwprintf(stderr, L"Input WAV not found: %s\n", opt.inPath.c_str());
        return EXIT_INPUT_NOT_FOUND;
    }

    std::vector<unsigned char> inputBytes;
    if (!ReadAllBytes(opt.inPath, inputBytes))
    {
        std::fwprintf(stderr, L"Failed to read input WAV: %s\n", opt.inPath.c_str());
        return EXIT_READ_FAILED;
    }

    SWavData wav;
    if (!ParseWav(inputBytes, wav))
    {
        // 非対応形式は壊さずそのままコピーする。SAPI標準WAVは通常16bit PCMのため、通常ここには入らない。
        if (::CopyFileW(opt.inPath.c_str(), opt.outPath.c_str(), FALSE))
        {
            std::fwprintf(stdout, L"Unsupported WAV format. Copied without processing.\n");
            return EXIT_OK;
        }
        std::fwprintf(stderr, L"Unsupported WAV format and copy failed.\n");
        return EXIT_READ_FAILED;
    }

    SEffectSettings settings = LoadEffectSettings(opt.configPath, opt.presetName);
    ApplyEffectSettingOverrides(settings, opt.overrides);
    ProcessWav(wav, settings);

    std::vector<unsigned char> outputBytes = BuildWavBytes(wav);
    if (!WriteAllBytes(opt.outPath, outputBytes))
    {
        std::fwprintf(stderr, L"Failed to write output WAV: %s\n", opt.outPath.c_str());
        return EXIT_WRITE_FAILED;
    }

    std::fwprintf(stdout, L"WAV shaping completed: %s\n", opt.outPath.c_str());
    return EXIT_OK;
}
