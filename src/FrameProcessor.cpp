#include "App/FrameProcessor.h"

#include <opencv2/imgproc.hpp>
#include <chrono>
#include <cstring>

namespace fightsight {

namespace {
    inline double MillisSince(std::chrono::high_resolution_clock::time_point t0) {
        const auto t1 = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
}

FrameProcessor::~FrameProcessor() {
    Shutdown();
}

bool FrameProcessor::Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    m_device  = device;
    m_context = context;
    return true;
}

void FrameProcessor::Shutdown() {
    ReleaseDisplay();
    ReleaseStaging();
    m_processed.release();
    m_device  = nullptr;
    m_context = nullptr;
}

bool FrameProcessor::Process(ID3D11Texture2D* sourceTex, bool force1080p) {
    if (!sourceTex || !m_device || !m_context) return false;

    D3D11_TEXTURE2D_DESC srcDesc{};
    sourceTex->GetDesc(&srcDesc);

    if (srcDesc.Width == 0 || srcDesc.Height == 0) return false;
    if (!EnsureStaging(static_cast<int>(srcDesc.Width),
                       static_cast<int>(srcDesc.Height),
                       srcDesc.Format)) {
        return false;
    }

    // ---- 1) GPU copy + sync readback into the staging texture ----
    const auto tReadback0 = std::chrono::high_resolution_clock::now();
    m_context->CopyResource(m_staging, sourceTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;
    m_lastReadbackMs = MillisSince(tReadback0);

    // ---- 2) Zero-copy cv::Mat header over the mapped GPU memory ----
    // RowPitch (not width*4) is mandatory - D3D often pads rows for alignment.
    cv::Mat srcView(
        static_cast<int>(srcDesc.Height),
        static_cast<int>(srcDesc.Width),
        CV_8UC4,
        mapped.pData,
        static_cast<size_t>(mapped.RowPitch)
    );

    // ---- 3) Normalize to 1080p if requested; otherwise copy out as-is ----
    int dstW = static_cast<int>(srcDesc.Width);
    int dstH = static_cast<int>(srcDesc.Height);
    const bool needResize = force1080p &&
                            (srcDesc.Width != 1920 || srcDesc.Height != 1080);

    // Lock only around the WRITE to m_processed. The display-upload below
    // also reads m_processed but happens on this thread, so it's sequenced
    // with the write. Concurrent reads from the CV worker's Snapshot are
    // safe (two reads never race in cv::Mat).
    {
        std::lock_guard<std::mutex> lk(m_processedMutex);
        if (needResize) {
            const auto tResize0 = std::chrono::high_resolution_clock::now();
            cv::resize(srcView, m_processed, cv::Size(1920, 1080),
                       0.0, 0.0, cv::INTER_LINEAR);
            m_lastResizeMs = MillisSince(tResize0);
            dstW = 1920;
            dstH = 1080;
        } else {
            // Mandatory: copy out of mapped memory before Unmap. We persist
            // the buffer in m_processed; cv::Mat::copyTo reuses storage if
            // size/type match - memcpy after the first frame at given size.
            srcView.copyTo(m_processed);
            m_lastResizeMs = 0.0;
        }
        m_lastSourceFormat = srcDesc.Format;
    }

    m_context->Unmap(m_staging, 0);

    // ---- 4) Upload processed Mat into the dynamic display texture ----
    const auto tUpload0 = std::chrono::high_resolution_clock::now();
    if (!EnsureDisplay(dstW, dstH, srcDesc.Format)) return false;

    D3D11_MAPPED_SUBRESOURCE dstMap{};
    hr = m_context->Map(m_display, 0, D3D11_MAP_WRITE_DISCARD, 0, &dstMap);
    if (FAILED(hr)) return false;

    // Row-wise copy honoring BOTH the Mat step and the destination RowPitch.
    const uint8_t* srcRow  = m_processed.data;
    uint8_t*       dstRow  = static_cast<uint8_t*>(dstMap.pData);
    const size_t   rowSize = static_cast<size_t>(dstW) * 4u;
    for (int y = 0; y < dstH; ++y) {
        std::memcpy(dstRow, srcRow, rowSize);
        srcRow += m_processed.step;
        dstRow += dstMap.RowPitch;
    }

    m_context->Unmap(m_display, 0);
    m_lastUploadMs = MillisSince(tUpload0);
    return true;
}

bool FrameProcessor::EnsureStaging(int w, int h, DXGI_FORMAT fmt) {
    if (m_staging && m_stagingW == w && m_stagingH == h && m_stagingFmt == fmt) {
        return true;
    }
    ReleaseStaging();

    D3D11_TEXTURE2D_DESC d{};
    d.Width              = static_cast<UINT>(w);
    d.Height             = static_cast<UINT>(h);
    d.MipLevels          = 1;
    d.ArraySize          = 1;
    d.Format             = fmt;
    d.SampleDesc.Count   = 1;
    d.SampleDesc.Quality = 0;
    d.Usage              = D3D11_USAGE_STAGING;
    d.BindFlags          = 0;
    d.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    d.MiscFlags          = 0;

    const HRESULT hr = m_device->CreateTexture2D(&d, nullptr, &m_staging);
    if (FAILED(hr)) {
        m_staging = nullptr;
        return false;
    }
    m_stagingW   = w;
    m_stagingH   = h;
    m_stagingFmt = fmt;
    return true;
}

bool FrameProcessor::EnsureDisplay(int w, int h, DXGI_FORMAT fmt) {
    if (m_display && m_displayW == w && m_displayH == h && m_displayFmt == fmt) {
        return true;
    }
    ReleaseDisplay();

    D3D11_TEXTURE2D_DESC d{};
    d.Width              = static_cast<UINT>(w);
    d.Height             = static_cast<UINT>(h);
    d.MipLevels          = 1;
    d.ArraySize          = 1;
    d.Format             = fmt;
    d.SampleDesc.Count   = 1;
    d.SampleDesc.Quality = 0;
    d.Usage              = D3D11_USAGE_DYNAMIC;
    d.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;
    d.MiscFlags          = 0;

    HRESULT hr = m_device->CreateTexture2D(&d, nullptr, &m_display);
    if (FAILED(hr)) {
        m_display = nullptr;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                    = fmt;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels       = 1;

    hr = m_device->CreateShaderResourceView(m_display, &srvDesc, &m_displaySRV);
    if (FAILED(hr)) {
        m_display->Release();
        m_display    = nullptr;
        m_displaySRV = nullptr;
        return false;
    }

    m_displayW   = w;
    m_displayH   = h;
    m_displayFmt = fmt;
    return true;
}

bool FrameProcessor::SnapshotProcessedFrame(cv::Mat& out) const {
    std::lock_guard<std::mutex> lk(m_processedMutex);
    if (m_processed.empty()) return false;
    m_processed.copyTo(out);
    return true;
}

void FrameProcessor::ReleaseStaging() {
    if (m_staging) { m_staging->Release(); m_staging = nullptr; }
    m_stagingW = m_stagingH = 0;
    m_stagingFmt = DXGI_FORMAT_UNKNOWN;
}

void FrameProcessor::ReleaseDisplay() {
    if (m_displaySRV) { m_displaySRV->Release(); m_displaySRV = nullptr; }
    if (m_display)    { m_display->Release();    m_display    = nullptr; }
    m_displayW = m_displayH = 0;
    m_displayFmt = DXGI_FORMAT_UNKNOWN;
}

} // namespace fightsight
