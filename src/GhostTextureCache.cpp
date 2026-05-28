#include "App/GhostTextureCache.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace fightsight {

namespace {
    void ReleaseEntry(GhostTextureCache* /*self*/,
                      ID3D11Texture2D*& tex,
                      ID3D11ShaderResourceView*& srv) {
        if (srv) { srv->Release(); srv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }
}

ID3D11ShaderResourceView* GhostTextureCache::GetSRV(const std::string& path) {
    if (!m_device) return nullptr;

    auto it = m_cache.find(path);
    if (it != m_cache.end()) return it->second.srv; // may be nullptr (cached miss)

    Entry& e = m_cache[path]; // inserts default-constructed entry

    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) return nullptr; // cached miss stays

    // Normalize to BGRA8 - matches our display tex format, and ColorButton/
    // AddImage modulation works correctly with the alpha channel from the
    // original PNG (so user-punched transparency shows through).
    if (img.channels() == 3) {
        cv::cvtColor(img, img, cv::COLOR_BGR2BGRA);
    } else if (img.channels() == 1) {
        cv::cvtColor(img, img, cv::COLOR_GRAY2BGRA);
    }
    if (img.channels() != 4) return nullptr;

    // Sanity clamp - refuse pathological sizes that would OOM the GPU.
    if (img.cols <= 0 || img.rows <= 0
        || img.cols > 8192 || img.rows > 8192) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(img.cols);
    td.Height           = static_cast<UINT>(img.rows);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = img.data;
    init.SysMemPitch = static_cast<UINT>(img.step);

    HRESULT hr = m_device->CreateTexture2D(&td, &init, &e.tex);
    if (FAILED(hr) || !e.tex) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels       = 1;

    hr = m_device->CreateShaderResourceView(e.tex, &sd, &e.srv);
    if (FAILED(hr) || !e.srv) {
        e.tex->Release();
        e.tex = nullptr;
        return nullptr;
    }
    e.w = img.cols;
    e.h = img.rows;
    return e.srv;
}

void GhostTextureCache::Invalidate(const std::string& path) {
    auto it = m_cache.find(path);
    if (it == m_cache.end()) return;
    ReleaseEntry(this, it->second.tex, it->second.srv);
    m_cache.erase(it);
}

void GhostTextureCache::Clear() {
    for (auto& kv : m_cache) {
        ReleaseEntry(this, kv.second.tex, kv.second.srv);
    }
    m_cache.clear();
}

} // namespace fightsight
