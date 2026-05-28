#pragma once

#include <d3d11.h>
#include <dxgiformat.h>
#include <mutex>
#include <opencv2/core.hpp>

namespace fightsight {

// Owns the DX11 <-> OpenCV pixel round-trip:
//   sender tex  --CopyResource-->  staging tex (CPU_READ)
//   Map(READ)   --> cv::Mat (zero-copy header over mapped memory)
//   optional cv::resize to 1920x1080
//   Map(WRITE_DISCARD) into a DYNAMIC display tex; SRV exposed for ImGui.
//
// Thread-safety: m_processed is mutex-protected for cross-thread snapshot
// (the CV worker calls SnapshotProcessedFrame from another thread).
class FrameProcessor {
public:
    FrameProcessor() = default;
    ~FrameProcessor();

    FrameProcessor(const FrameProcessor&) = delete;
    FrameProcessor& operator=(const FrameProcessor&) = delete;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Called once per render frame from the render thread.
    bool Process(ID3D11Texture2D* sourceTex, bool force1080p);

    // Output for ImGui. Null until the first successful Process() call.
    ID3D11ShaderResourceView* GetDisplaySRV() const { return m_displaySRV; }
    int GetDisplayWidth()  const { return m_displayW; }
    int GetDisplayHeight() const { return m_displayH; }

    // Per-frame timings (milliseconds) for the last Process() call.
    double GetLastReadbackMs() const { return m_lastReadbackMs; }
    double GetLastResizeMs()   const { return m_lastResizeMs;   }
    double GetLastUploadMs()   const { return m_lastUploadMs;   }

    // -----------------------------------------------------------------------
    // Thread-safe accessors for the CV worker / template capture.
    // -----------------------------------------------------------------------

    // Locks briefly, clones the latest processed Mat into `out`.
    // Returns false if no frame has been processed yet.
    bool SnapshotProcessedFrame(cv::Mat& out) const;

    // DXGI format of the most recent source texture. Used by consumers to
    // know whether to swap R<->B before saving/comparing.
    DXGI_FORMAT GetLastSourceFormat() const { return m_lastSourceFormat; }
    bool        IsSenderRGBA() const {
        return m_lastSourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM
            || m_lastSourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            || m_lastSourceFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    }

private:
    bool EnsureStaging(int w, int h, DXGI_FORMAT fmt);
    bool EnsureDisplay(int w, int h, DXGI_FORMAT fmt);
    void ReleaseStaging();
    void ReleaseDisplay();

    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    ID3D11Texture2D* m_staging    = nullptr;
    int              m_stagingW   = 0;
    int              m_stagingH   = 0;
    DXGI_FORMAT      m_stagingFmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D*          m_display    = nullptr;
    ID3D11ShaderResourceView* m_displaySRV = nullptr;
    int                       m_displayW   = 0;
    int                       m_displayH   = 0;
    DXGI_FORMAT               m_displayFmt = DXGI_FORMAT_UNKNOWN;

    // Persistent processed-Mat storage. The mutex guards WRITES from the
    // render thread (Process) vs cross-thread reads (SnapshotProcessedFrame).
    // Two concurrent reads are safe, so the display-upload memcpy doesn't
    // need to hold the lock - it's same-thread sequenced with the write.
    mutable std::mutex m_processedMutex;
    cv::Mat            m_processed;
    DXGI_FORMAT        m_lastSourceFormat = DXGI_FORMAT_UNKNOWN;

    double m_lastReadbackMs = 0.0;
    double m_lastResizeMs   = 0.0;
    double m_lastUploadMs   = 0.0;
};

} // namespace fightsight
