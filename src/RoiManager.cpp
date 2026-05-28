#include "App/RoiManager.h"

#include <algorithm>

namespace fightsight {

std::vector<Roi> RoiManager::SnapshotAll() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_rois; // copy
}

Roi& RoiManager::Add(const std::string& name, RoiType type, const cv::Rect& rect) {
    Roi r;
    r.id   = m_nextId++;
    r.name = name;
    r.type = type;
    r.rect = rect;
    m_rois.push_back(std::move(r));
    return m_rois.back();
}

void RoiManager::Remove(int id) {
    m_rois.erase(
        std::remove_if(m_rois.begin(), m_rois.end(),
                       [id](const Roi& r) { return r.id == id; }),
        m_rois.end());
}

Roi* RoiManager::Find(int id) {
    for (auto& r : m_rois) if (r.id == id) return &r;
    return nullptr;
}

const Roi* RoiManager::Find(int id) const {
    for (const auto& r : m_rois) if (r.id == id) return &r;
    return nullptr;
}

void RoiManager::LoadFromJson(const nlohmann::json& arr) {
    m_rois.clear();
    m_nextId = 1;
    if (!arr.is_array()) return;
    for (const auto& j : arr) {
        try {
            Roi r = Roi::FromJson(j);
            if (r.id <= 0) r.id = m_nextId;
            m_nextId = std::max(m_nextId, r.id + 1);
            m_rois.push_back(std::move(r));
        } catch (const std::exception&) {
            // skip malformed
        }
    }
}

nlohmann::json RoiManager::ToJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : m_rois) arr.push_back(r.ToJson());
    return arr;
}

void RoiManager::Copy(int roiId) {
    for (const auto& r : m_rois) {
        if (r.id == roiId) {
            m_clipboard    = r;
            m_hasClipboard = true;
            return;
        }
    }
}

int RoiManager::PasteNew() {
    if (!m_hasClipboard) return 0;
    Roi r  = m_clipboard;
    r.id   = m_nextId++;
    r.name = m_clipboard.name + "_copy";
    // Offset the rect so the paste doesn't sit exactly on top of the
    // original - 20px feels right for hand-edit scale.
    r.rect.x += 20;
    r.rect.y += 20;
    m_rois.push_back(r);
    return r.id;
}

int RoiManager::PasteMirrored(int canonW) {
    if (!m_hasClipboard) return 0;
    Roi r  = m_clipboard;
    r.id   = m_nextId++;
    r.name = m_clipboard.name + "_mirror";
    // x' = canonW - x - w  (mirror around the vertical centerline).
    // Keep Y, W, H identical so a P1 health bar lands on P2's slot.
    r.rect.x = std::max(0, canonW - r.rect.x - r.rect.width);
    m_rois.push_back(r);
    return r.id;
}

} // namespace fightsight
