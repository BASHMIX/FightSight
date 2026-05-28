#pragma once

#include "App/Roi.h"
#include <mutex>
#include <vector>

namespace fightsight {

// Pure data layer. Owns the ROI list; no ImGui code, no DX code.
//
// Thread-safety convention:
//   - SnapshotAll() locks INTERNALLY and returns a copy (use from CV worker).
//   - Direct access methods (All(), Find(), Add(), Remove(), ToJson(),
//     LoadFromJson()) are UNLOCKED. The caller must hold Mutex() while
//     using them. The render thread acquires the lock around any block
//     that touches the ROI list (ROIs panel iteration, RoiEditor drag).
class RoiManager {
public:
    // ----- Thread-safe (locks internally) ---------------------------------
    std::vector<Roi> SnapshotAll() const;

    // ----- Unlocked accessors: caller must hold Mutex() -------------------
    std::vector<Roi>&       All()       { return m_rois; }
    const std::vector<Roi>& All() const { return m_rois; }

    Roi*       Find(int id);
    const Roi* Find(int id) const;

    Roi&  Add(const std::string& name, RoiType type, const cv::Rect& rect);
    void  Remove(int id);

    nlohmann::json ToJson() const;
    void           LoadFromJson(const nlohmann::json& arr);

    int NextId() const { return m_nextId; }

    std::mutex& Mutex() const { return m_mutex; }

    // ----- Clipboard (Phase 8) --------------------------------------------
    // Caller must hold Mutex() across all clipboard calls.
    bool HasClipboard() const { return m_hasClipboard; }
    void Copy(int roiId);
    // Paste a fresh copy of the clipboard with a new id and an offset rect
    // so it doesn't sit exactly on top of the original. Suffixes the name
    // with "_copy" so per-state template PNGs don't collide.
    // Returns the new id (or 0 if clipboard is empty).
    int  PasteNew();
    // Same, but flips the rect horizontally around `canonW`. Name gets
    // "_mirror" appended.
    int  PasteMirrored(int canonW);

private:
    std::vector<Roi>   m_rois;
    int                m_nextId = 1;
    mutable std::mutex m_mutex;

    Roi  m_clipboard;
    bool m_hasClipboard = false;
};

} // namespace fightsight
