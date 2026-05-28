#pragma once

#include <d3d11.h>

#include <string>
#include <unordered_map>

namespace fightsight {

// Path-keyed cache of GPU textures for template PNGs used by the ROI
// editor's "Ghost Overlay" alignment helper.
//
// Render-thread-only. Loads on demand via cv::imread + CreateTexture2D;
// failed loads are also cached (as null SRV) so we don't hit the disk
// every frame for a missing template.
class GhostTextureCache {
public:
    GhostTextureCache() = default;
    ~GhostTextureCache() { Shutdown(); }

    GhostTextureCache(const GhostTextureCache&) = delete;
    GhostTextureCache& operator=(const GhostTextureCache&) = delete;

    void Init(ID3D11Device* device) { m_device = device; }
    void Shutdown() { Clear(); m_device = nullptr; }

    // Returns the cached SRV for `pngPath`, loading on first call.
    // Returns nullptr if the file is missing / unreadable / oversize.
    // (Subsequent calls for the same path also return nullptr without
    // re-reading; call Invalidate() after a re-capture.)
    ID3D11ShaderResourceView* GetSRV(const std::string& pngPath);

    // Drop the cache entry for one path (e.g. after the user re-captures
    // that ROI's template). Next GetSRV() reloads from disk.
    void Invalidate(const std::string& pngPath);

    // Drop everything (e.g. on profile switch - the templates dir changed
    // entirely so all previously-keyed paths are stale).
    void Clear();

private:
    struct Entry {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
    };

    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, Entry> m_cache;
};

} // namespace fightsight
