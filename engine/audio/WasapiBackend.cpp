#include "audio/WasapiBackend.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
// PKEY_Device_FriendlyName 在这个头里（mmdeviceapi.h 只给接口，不给属性键）
#include <functiondiscoverykeys_devpkey.h>

// ⚠ MinGW 坑：`<functiondiscoverykeys_devpkey.h>` 只在定义了 `INITGUID` 时
// 用 `DEFINE_PROPERTYKEY` 生成**定义**，否则它只展开成一句
// `extern "C" const PROPERTYKEY PKEY_Device_FriendlyName;`（见 propkeydef.h）。
// MSVC 下这些数据符号来自 SDK 的 propsys.lib；MinGW 的 libpropsys.a
// **不导出**它们 → 链接期报 `undefined reference to PKEY_Device_FriendlyName`
// （编译期完全看不出来）。这里本地补一份定义。
// `DECLSPEC_SELECTANY`(= __declspec(selectany)) 保证即使别处也定义了
// 也不会重复符号。值必须与 SDK 一致：{a45c254e-df1c-4efd-8020-67d146a850e0}, 14。
#if defined(__MINGW32__)
EXTERN_C const PROPERTYKEY DECLSPEC_SELECTANY PKEY_Device_FriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};
#endif

#include <algorithm>
#include <cstring>

namespace audio {

// ------------------------------------------------------------
// COM 接口容器（头文件里不出现 windows.h）
// ------------------------------------------------------------
struct WasapiBackend::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    bool comInited = false;
    bool isFloat = true;
    std::uint16_t bits = 32;
};

namespace {

template <typename T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// 设备友好名（IPropertyStore → PKEY_Device_FriendlyName）
std::string friendlyName(IMMDevice* dev) {
    if (!dev) return {};
    IPropertyStore* store = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &v)) &&
        v.vt == VT_LPWSTR && v.pwszVal) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr,
                                             0, nullptr, nullptr);
        if (need > 0) {
            out.resize(static_cast<std::size_t>(need - 1));
            WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out.data(), need,
                                nullptr, nullptr);
        }
    }
    PropVariantClear(&v);
    store->Release();
    return out;
}

bool containsNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    std::string a = hay, b = needle;
    for (char& c : a) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : b) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return a.find(b) != std::string::npos;
}

} // namespace

WasapiBackend::~WasapiBackend() { close(); }

bool WasapiBackend::open(const OpenParams& params, MixFn fn, void* user,
                         std::string& err) {
    if (m_open) close();
    if (!fn) {
        err = "no-mix-callback";
        return false;
    }
    m_fn = fn;
    m_user = user;
    m_fellBack = false;
    m_lost.store(false);

    m_impl = new Impl();

    // ⚠ RPC_E_CHANGED_MODE = 别的模块已经用 STA 初始化过 —— 不算失败，
    //   只是我们这个线程不能再用 MTA。仍然可以创建对象，所以忽略即可。
    const HRESULT hrInit =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_impl->comInited = SUCCEEDED(hrInit);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) {
        err = "com-init-failed";
        releaseAll();
        return false;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&m_impl->enumerator));
    if (FAILED(hr) || !m_impl->enumerator) {
        err = "no-enumerator";
        releaseAll();
        return false;
    }

    // ---- 选设备 ----
    const std::string want = params.deviceName;
    const bool wantDefault =
        want.empty() || want == "default" || want == "default-device";
    bool matched = false;

    if (!wantDefault) {
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(m_impl->enumerator->EnumAudioEndpoints(
                eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count && !matched; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(coll->Item(i, &d)) || !d) continue;
                const std::string nm = friendlyName(d);
                LPWSTR id = nullptr;
                std::string idStr;
                if (SUCCEEDED(d->GetId(&id)) && id) {
                    const int need = WideCharToMultiByte(CP_UTF8, 0, id, -1,
                                                         nullptr, 0, nullptr,
                                                         nullptr);
                    if (need > 0) {
                        idStr.resize(static_cast<std::size_t>(need - 1));
                        WideCharToMultiByte(CP_UTF8, 0, id, -1, idStr.data(),
                                            need, nullptr, nullptr);
                    }
                    CoTaskMemFree(id);
                }
                if (containsNoCase(nm, want) || containsNoCase(idStr, want)) {
                    m_impl->device = d;
                    m_deviceName = nm;
                    matched = true;
                } else {
                    d->Release();
                }
            }
            coll->Release();
        }
    }

    if (!matched) {
        // 默认设备（或"名字没匹配上"的回落）。⚠ 回落必须**留痕**，
        // 否则用户以为 game.json 里的 audioDevice 生效了（R9）。
        if (!wantDefault) m_fellBack = true;
        hr = m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                         &m_impl->device);
        if (FAILED(hr) || !m_impl->device) {
            err = "no-default-device";
            releaseAll();
            return false;
        }
        m_deviceName = friendlyName(m_impl->device);
    }

    // ---- 激活 IAudioClient ----
    hr = m_impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&m_impl->client));
    if (FAILED(hr) || !m_impl->client) {
        err = "activate-failed";
        releaseAll();
        return false;
    }

    // ⚠ 永远先问设备要它的 mix format（R3）。强行设 44.1 kHz 会失败，
    //   或者悄悄被重采样成别的样子 → 变调。
    hr = m_impl->client->GetMixFormat(&m_impl->mixFormat);
    if (FAILED(hr) || !m_impl->mixFormat) {
        err = "no-mix-format";
        releaseAll();
        return false;
    }

    WAVEFORMATEX* wf = m_impl->mixFormat;
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
        tag = ext->SubFormat.Data1 == 3 ? 3 : (ext->SubFormat.Data1 == 1 ? 1 : tag);
    }
    m_impl->isFloat = (tag == 3);
    m_impl->bits = wf->wBitsPerSample;
    if (!m_impl->isFloat && !(tag == 1 && wf->wBitsPerSample == 16)) {
        err = "unsupported-mix-format";
        releaseAll();
        return false;
    }

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    m_impl->client->GetDevicePeriod(&defaultPeriod, &minPeriod);
    if (defaultPeriod <= 0) defaultPeriod = 100000;   // 10 ms 兜底

    // 4 个周期 = 40 ms 的缓冲。够稳，又不会让"暂停/播放"有明显延迟。
    const REFERENCE_TIME bufferDuration = defaultPeriod * 4;

    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) {
        err = "create-event-failed";
        releaseAll();
        return false;
    }
    hr = m_impl->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufferDuration, 0, m_impl->mixFormat, nullptr);
    if (FAILED(hr)) {
        // 最后再退一步：不用事件回调，纯周期驱动（某些虚拟设备只支持这个）
        hr = m_impl->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                        bufferDuration, 0, m_impl->mixFormat,
                                        nullptr);
        if (FAILED(hr)) {
            err = "initialize-failed";
            releaseAll();
            return false;
        }
    }
    m_impl->client->SetEventHandle(static_cast<HANDLE>(m_event));

    UINT32 bufferFrames = 0;
    m_impl->client->GetBufferSize(&bufferFrames);
    m_bufferFrames = bufferFrames;
    m_periodFrames = static_cast<std::uint32_t>(
        (static_cast<double>(defaultPeriod) * 1e-7) * wf->nSamplesPerSec);
    if (m_periodFrames == 0 || m_periodFrames > bufferFrames)
        m_periodFrames = bufferFrames ? bufferFrames : 480;

    hr = m_impl->client->GetService(__uuidof(IAudioRenderClient),
                                    reinterpret_cast<void**>(&m_impl->render));
    if (FAILED(hr) || !m_impl->render) {
        err = "no-render-client";
        releaseAll();
        return false;
    }

    m_format.sampleRate = wf->nSamplesPerSec;
    m_format.channels = wf->nChannels;
    m_format.frames = 0;

    m_convert.assign(static_cast<std::size_t>(m_bufferFrames) *
                         m_format.channels,
                     0.0f);

    m_open = true;
    err.clear();
    return true;
}

bool WasapiBackend::start() {
    if (!m_open || m_running.load()) return m_open;
    if (m_impl && m_impl->client) {
        const HRESULT hr = m_impl->client->Start();
        if (FAILED(hr)) return false;
    }
    m_running.store(true);
    m_thread = std::thread(&WasapiBackend::threadLoop, this);
    return true;
}

void WasapiBackend::stop() {
    m_running.store(false);
    if (m_event) SetEvent(static_cast<HANDLE>(m_event));
    if (m_thread.joinable()) m_thread.join();
    if (m_impl && m_impl->client) m_impl->client->Stop();
}

void WasapiBackend::close() {
    stop();
    releaseAll();
}

void WasapiBackend::releaseAll() {
    if (m_event) {
        CloseHandle(static_cast<HANDLE>(m_event));
        m_event = nullptr;
    }
    m_open = false;
    if (!m_impl) return;
    safeRelease(m_impl->render);
    safeRelease(m_impl->client);
    safeRelease(m_impl->device);
    safeRelease(m_impl->enumerator);
    if (m_impl->mixFormat) {
        CoTaskMemFree(m_impl->mixFormat);
        m_impl->mixFormat = nullptr;
    }
    if (m_impl->comInited) CoUninitialize();
    delete m_impl;
    m_impl = nullptr;
}

double WasapiBackend::blockDurationSeconds() const {
    if (!m_format.sampleRate) return 0.0;
    return static_cast<double>(m_periodFrames) /
           static_cast<double>(m_format.sampleRate);
}

int WasapiBackend::enumerate(std::vector<DeviceInfo>& out) const {
    out.clear();
    Impl tmp;
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool inited = SUCCEEDED(hrInit);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&tmp.enumerator));
    if (FAILED(hr) || !tmp.enumerator) {
        if (inited) CoUninitialize();
        return 0;
    }
    IMMDevice* def = nullptr;
    std::string defName;
    if (SUCCEEDED(tmp.enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                          &def)) &&
        def) {
        defName = friendlyName(def);
        def->Release();
    }
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(tmp.enumerator->EnumAudioEndpoints(eRender,
                                                     DEVICE_STATE_ACTIVE,
                                                     &coll)) &&
        coll) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d)) || !d) continue;
            DeviceInfo info;
            info.name = friendlyName(d);
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                const int need = WideCharToMultiByte(CP_UTF8, 0, id, -1, nullptr,
                                                     0, nullptr, nullptr);
                if (need > 0) {
                    info.id.resize(static_cast<std::size_t>(need - 1));
                    WideCharToMultiByte(CP_UTF8, 0, id, -1, info.id.data(), need,
                                        nullptr, nullptr);
                }
                CoTaskMemFree(id);
            }
            info.isDefault = !defName.empty() && info.name == defName;
            out.push_back(info);
            d->Release();
        }
        coll->Release();
    }
    tmp.enumerator->Release();
    if (inited) CoUninitialize();
    return static_cast<int>(out.size());
}

void WasapiBackend::threadLoop() {
    // 音频线程也要初始化 COM（MTA），否则 IAudioClient 的调用会失败。
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool inited = SUCCEEDED(hrInit);

    const std::uint32_t ch = m_format.channels;
    const std::uint32_t chunk = m_periodFrames ? m_periodFrames : 480;

    while (m_running.load()) {
        if (m_event) {
            // 超时 = 200 ms：设备卡死时也要能退出（不能无限等）。
            const DWORD w = WaitForSingleObject(static_cast<HANDLE>(m_event), 200);
            if (!m_running.load()) break;
            if (w != WAIT_OBJECT_0) continue;
        }

        UINT32 padding = 0;
        if (FAILED(m_impl->client->GetCurrentPadding(&padding))) {
            m_lost.store(true);
            break;
        }
        UINT32 avail = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
        if (avail == 0) continue;
        if (avail > chunk) avail = chunk;

        BYTE* data = nullptr;
        const HRESULT hr = m_impl->render->GetBuffer(avail, &data);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            m_lost.store(true);
            break;
        }
        if (FAILED(hr) || !data) continue;

        if (m_impl->isFloat) {
            // ⚠ 回调必须把 avail*ch 个 float **全部写满**（后端不预清零）
            m_fn(m_user, reinterpret_cast<float*>(data), avail, ch);
        } else {
            // 16-bit PCM 的 mix format（少见，但虚拟声卡会有）
            m_fn(m_user, m_convert.data(), avail, ch);
            auto* dst = reinterpret_cast<std::int16_t*>(data);
            const std::size_t n = static_cast<std::size_t>(avail) * ch;
            for (std::size_t i = 0; i < n; ++i) {
                float v = m_convert[i];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                dst[i] = static_cast<std::int16_t>(v * 32767.0f);
            }
        }
        const HRESULT hrRel = m_impl->render->ReleaseBuffer(avail, 0);
        if (hrRel == AUDCLNT_E_DEVICE_INVALIDATED) {
            m_lost.store(true);
            break;
        }
    }

    if (inited) CoUninitialize();
}

} // namespace audio

#endif // _WIN32
