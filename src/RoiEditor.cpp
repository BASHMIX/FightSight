#include "App/RoiEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fightsight {

namespace {
    constexpr float kGrabPxScreen   = 8.0f; // grab slack, screen px
    constexpr float kHandleHalfPx   = 5.0f; // handle square half-size, screen px
    constexpr int   kMinRectSizePx  = 4;    // smallest committable ROI

    template <typename T>
    T clamp_(T v, T lo, T hi) { return std::max(lo, std::min(v, hi)); }
}

// ---------------------------------------------------------------------------
// Coordinate transforms (Phase 8: zoom + pan aware)
// ---------------------------------------------------------------------------
// At zoom = Z and panUV = (px, py), the visible canon rect is:
//   origin = (px*canonW, py*canonH)
//   size   = (canonW/Z, canonH/Z)
// The display rect (m_imgMin..m_imgMax) shows that visible canon region.
// When Z=1 and panUV=(0,0) the math collapses to the pre-Phase-8 identity.
cv::Point RoiEditor::ScreenToCanon(const ImVec2& s) const {
    const float dispW = m_imgMax.x - m_imgMin.x;
    const float dispH = m_imgMax.y - m_imgMin.y;
    if (dispW <= 0.0f || dispH <= 0.0f) return {0, 0};
    const float z    = (m_zoom > 0.0f) ? m_zoom : 1.0f;
    const float visW = static_cast<float>(m_canonW) / z;
    const float visH = static_cast<float>(m_canonH) / z;
    const float fx   = (s.x - m_imgMin.x) / dispW;
    const float fy   = (s.y - m_imgMin.y) / dispH;
    const float cx   = m_panUV.x * m_canonW + fx * visW;
    const float cy   = m_panUV.y * m_canonH + fy * visH;
    return cv::Point(static_cast<int>(std::round(cx)),
                     static_cast<int>(std::round(cy)));
}

ImVec2 RoiEditor::CanonToScreen(int x, int y) const {
    const float dispW = m_imgMax.x - m_imgMin.x;
    const float dispH = m_imgMax.y - m_imgMin.y;
    if (m_canonW == 0 || m_canonH == 0) return m_imgMin;
    const float z    = (m_zoom > 0.0f) ? m_zoom : 1.0f;
    const float visW = static_cast<float>(m_canonW) / z;
    const float visH = static_cast<float>(m_canonH) / z;
    if (visW <= 0.0f || visH <= 0.0f) return m_imgMin;
    const float fx   = (static_cast<float>(x) - m_panUV.x * m_canonW) / visW;
    const float fy   = (static_cast<float>(y) - m_panUV.y * m_canonH) / visH;
    return ImVec2(m_imgMin.x + fx * dispW,
                  m_imgMin.y + fy * dispH);
}

// ---------------------------------------------------------------------------
// Visual helpers
// ---------------------------------------------------------------------------
ImU32 RoiEditor::ColorForType(RoiType t) {
    switch (t) {
        case RoiType::Text:     return IM_COL32(  0, 200, 255, 255);
        case RoiType::Gradient: return IM_COL32(255, 220,   0, 255);
        case RoiType::Pixel:    return IM_COL32(255,  60, 220, 255);
        case RoiType::OcrZone:  return IM_COL32(120, 255, 120, 255); // green = container
    }
    return IM_COL32(200, 200, 200, 255);
}

ImGuiMouseCursor RoiEditor::CursorForHandle(Handle h) {
    switch (h) {
        case Handle::TopLeft:
        case Handle::BottomRight: return ImGuiMouseCursor_ResizeNWSE;
        case Handle::TopRight:
        case Handle::BottomLeft:  return ImGuiMouseCursor_ResizeNESW;
        case Handle::Top:
        case Handle::Bottom:      return ImGuiMouseCursor_ResizeNS;
        case Handle::Left:
        case Handle::Right:       return ImGuiMouseCursor_ResizeEW;
        case Handle::Body:        return ImGuiMouseCursor_ResizeAll;
        default:                  return ImGuiMouseCursor_Arrow;
    }
}

void RoiEditor::DrawHandles(ImDrawList* dl, const cv::Rect& r, ImU32 color) {
    const int x1 = r.x,             y1 = r.y;
    const int x2 = r.x + r.width,   y2 = r.y + r.height;
    const int xm = r.x + r.width  / 2;
    const int ym = r.y + r.height / 2;
    const ImU32 outline = IM_COL32(0, 0, 0, 220);

    auto drawAt = [&](int cx, int cy) {
        const ImVec2 p = CanonToScreen(cx, cy);
        const ImVec2 a{p.x - kHandleHalfPx, p.y - kHandleHalfPx};
        const ImVec2 b{p.x + kHandleHalfPx, p.y + kHandleHalfPx};
        dl->AddRectFilled(a, b, color);
        dl->AddRect      (a, b, outline);
    };

    drawAt(x1, y1); drawAt(xm, y1); drawAt(x2, y1);
    drawAt(x1, ym);                 drawAt(x2, ym);
    drawAt(x1, y2); drawAt(xm, y2); drawAt(x2, y2);
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------
RoiEditor::Handle RoiEditor::HitTestRoi(const cv::Rect& r,
                                        const cv::Point& pt,
                                        float tolCanon) {
    const int x1 = r.x,             y1 = r.y;
    const int x2 = r.x + r.width,   y2 = r.y + r.height;

    const bool nearLeft   = std::abs(pt.x - x1) <= tolCanon;
    const bool nearRight  = std::abs(pt.x - x2) <= tolCanon;
    const bool nearTop    = std::abs(pt.y - y1) <= tolCanon;
    const bool nearBottom = std::abs(pt.y - y2) <= tolCanon;
    const bool inXRange   = pt.x >= x1 - tolCanon && pt.x <= x2 + tolCanon;
    const bool inYRange   = pt.y >= y1 - tolCanon && pt.y <= y2 + tolCanon;

    // Corners first - they overlap with edges, and corner > edge in priority.
    if (nearLeft  && nearTop)    return Handle::TopLeft;
    if (nearRight && nearTop)    return Handle::TopRight;
    if (nearLeft  && nearBottom) return Handle::BottomLeft;
    if (nearRight && nearBottom) return Handle::BottomRight;
    // Edges (along the full length of the edge, not just midpoint)
    if (nearTop    && inXRange)  return Handle::Top;
    if (nearBottom && inXRange)  return Handle::Bottom;
    if (nearLeft   && inYRange)  return Handle::Left;
    if (nearRight  && inYRange)  return Handle::Right;
    // Body last
    if (pt.x >= x1 && pt.x <= x2 && pt.y >= y1 && pt.y <= y2) return Handle::Body;
    return Handle::None;
}

// ---------------------------------------------------------------------------
// Drag math
// ---------------------------------------------------------------------------
void RoiEditor::ApplyHandleDrag(cv::Rect& out,
                                const cv::Rect& orig,
                                const cv::Point& d,
                                Handle h,
                                int canonW, int canonH) {
    int x1 = orig.x;
    int y1 = orig.y;
    int x2 = orig.x + orig.width;
    int y2 = orig.y + orig.height;

    switch (h) {
        case Handle::TopLeft:     x1 += d.x; y1 += d.y; break;
        case Handle::Top:                    y1 += d.y; break;
        case Handle::TopRight:    x2 += d.x; y1 += d.y; break;
        case Handle::Right:       x2 += d.x;             break;
        case Handle::BottomRight: x2 += d.x; y2 += d.y; break;
        case Handle::Bottom:                 y2 += d.y; break;
        case Handle::BottomLeft:  x1 += d.x; y2 += d.y; break;
        case Handle::Left:        x1 += d.x;             break;
        default: break;
    }

    // Normalize inverted drag (e.g. user dragged TopLeft past BottomRight).
    int nx1 = std::min(x1, x2);
    int ny1 = std::min(y1, y2);
    int nx2 = std::max(x1, x2);
    int ny2 = std::max(y1, y2);

    nx1 = clamp_(nx1, 0, canonW);
    ny1 = clamp_(ny1, 0, canonH);
    nx2 = clamp_(nx2, 0, canonW);
    ny2 = clamp_(ny2, 0, canonH);

    out = cv::Rect(nx1, ny1, nx2 - nx1, ny2 - ny1);
}

// ---------------------------------------------------------------------------
// Main per-frame entry point
// ---------------------------------------------------------------------------
void RoiEditor::DrawAndHandleInput(const ImVec2& imgScreenMin,
                                   const ImVec2& imgScreenMax,
                                   int canonW, int canonH,
                                   float zoomLevel,
                                   const ImVec2& panUV) {
    std::lock_guard<std::mutex> lk(m_mgr.Mutex());

    m_imgMin = imgScreenMin;
    m_imgMax = imgScreenMax;
    m_canonW = canonW;
    m_canonH = canonH;
    m_zoom   = (zoomLevel > 0.0f) ? zoomLevel : 1.0f;
    m_panUV  = panUV;

    const float dispW = imgScreenMax.x - imgScreenMin.x;
    const float dispH = imgScreenMax.y - imgScreenMin.y;
    if (dispW <= 0.0f || dispH <= 0.0f || canonW <= 0 || canonH <= 0) return;

    // The grab slack is constant in SCREEN pixels (so handles feel the
    // same size regardless of zoom). Convert to canon using the zoomed
    // visible scale.
    const float canonPerScreen =
        static_cast<float>(canonW) / (dispW * m_zoom);
    const float tolCanon       = kGrabPxScreen * canonPerScreen;

    // --- Invisible button overlay for proper input handling ---
    // ImGui::Image has item-id 0 and is unreliable for clicks/hover, so we
    // claim the same rect with an InvisibleButton sitting on top.
    ImGui::SetCursorScreenPos(imgScreenMin);
    ImGui::InvisibleButton("##roi-canvas", ImVec2(dispW, dispH),
                           ImGuiButtonFlags_MouseButtonLeft);

    const bool   hovering    = ImGui::IsItemHovered();
    const bool   clicked     = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 mouseScreen = ImGui::GetIO().MousePos;
    const cv::Point mouseCanon = ScreenToCanon(mouseScreen);

    // --- Recompute hover state every idle frame ---
    if (m_mode == Mode::Idle) {
        m_hoveredId     = 0;
        m_hoveredHandle = Handle::None;
        if (hovering) {
            // Iterate in REVERSE so the top-most (drawn last) ROI wins.
            for (auto it = m_mgr.All().rbegin(); it != m_mgr.All().rend(); ++it) {
                Handle h = HitTestRoi(it->rect, mouseCanon, tolCanon);
                if (h != Handle::None) {
                    m_hoveredId     = it->id;
                    m_hoveredHandle = h;
                    break;
                }
            }
            if (m_hoveredHandle != Handle::None) {
                ImGui::SetMouseCursor(CursorForHandle(m_hoveredHandle));
            }
        }
    }

    // --- Click: start an interaction ---
    if (clicked) {
        if (m_hoveredId != 0 && m_hoveredHandle != Handle::None) {
            // Grab an existing ROI's handle/body
            m_selectedId       = m_hoveredId;
            m_dragHandle       = m_hoveredHandle;
            m_dragAnchor       = mouseCanon;
            if (const Roi* r = m_mgr.Find(m_selectedId)) {
                m_dragOriginalRect = r->rect;
            }
            m_mode = (m_dragHandle == Handle::Body) ? Mode::Moving
                                                    : Mode::ResizingHandle;
        } else {
            // Start drawing a new ROI from empty canvas
            m_mode         = Mode::Drawing;
            m_dragAnchor   = mouseCanon;
            m_drawingRect  = cv::Rect(mouseCanon.x, mouseCanon.y, 0, 0);
            m_selectedId   = 0;
        }
    }

    // --- Active drag (use global IsMouseDown so drag-outside-image works) ---
    if (m_mode != Mode::Idle && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (m_mode == Mode::Drawing) {
            int x1 = std::min(m_dragAnchor.x, mouseCanon.x);
            int y1 = std::min(m_dragAnchor.y, mouseCanon.y);
            int x2 = std::max(m_dragAnchor.x, mouseCanon.x);
            int y2 = std::max(m_dragAnchor.y, mouseCanon.y);
            x1 = clamp_(x1, 0, canonW); y1 = clamp_(y1, 0, canonH);
            x2 = clamp_(x2, 0, canonW); y2 = clamp_(y2, 0, canonH);
            m_drawingRect = cv::Rect(x1, y1, x2 - x1, y2 - y1);
        }
        else if (m_mode == Mode::Moving) {
            const cv::Point delta = mouseCanon - m_dragAnchor;
            cv::Rect r = m_dragOriginalRect;
            r.x += delta.x;
            r.y += delta.y;
            r.x = clamp_(r.x, 0, canonW - r.width);
            r.y = clamp_(r.y, 0, canonH - r.height);
            if (Roi* roi = m_mgr.Find(m_selectedId)) roi->rect = r;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        else if (m_mode == Mode::ResizingHandle) {
            const cv::Point delta = mouseCanon - m_dragAnchor;
            cv::Rect out;
            ApplyHandleDrag(out, m_dragOriginalRect, delta, m_dragHandle,
                            canonW, canonH);
            if (Roi* roi = m_mgr.Find(m_selectedId)) roi->rect = out;
            ImGui::SetMouseCursor(CursorForHandle(m_dragHandle));
        }
    }

    // --- Release: commit ---
    if (m_mode != Mode::Idle && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_mode == Mode::Drawing) {
            if (m_drawingRect.width  >= kMinRectSizePx &&
                m_drawingRect.height >= kMinRectSizePx) {
                char nameBuf[32];
                std::snprintf(nameBuf, sizeof(nameBuf),
                              "ROI %d", m_mgr.NextId());
                Roi& added = m_mgr.Add(nameBuf, m_defaultType, m_drawingRect);
                added.threshold = DefaultThresholdFor(m_defaultType);
                m_selectedId = added.id;
            }
            m_drawingRect = cv::Rect();
        }
        m_mode       = Mode::Idle;
        m_dragHandle = Handle::None;
    }

    // -----------------------------------------------------------------------
    // Render: ROIs, then drawing-in-progress overlay.
    // Clip to the image rect so nothing leaks into surrounding ImGui chrome.
    // -----------------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(imgScreenMin, imgScreenMax, true);

    for (const auto& roi : m_mgr.All()) {
        const bool selected = (roi.id == m_selectedId);
        const bool hovered  = (roi.id == m_hoveredId);

        const ImU32 color = ColorForType(roi.type);
        const ImU32 fill  = (color & 0x00FFFFFF) | 0x28000000; // alpha 0x28

        const ImVec2 a = CanonToScreen(roi.rect.x, roi.rect.y);
        const ImVec2 b = CanonToScreen(roi.rect.x + roi.rect.width,
                                       roi.rect.y + roi.rect.height);

        dl->AddRectFilled(a, b, fill);
        dl->AddRect(a, b, color, 0.0f, 0, selected ? 2.5f : 1.5f);

        char label[160];
        std::snprintf(label, sizeof(label), "#%d %s [%s]",
                      roi.id, roi.name.c_str(), RoiTypeName(roi.type));
        // Faint text shadow for readability over bright video frames
        dl->AddText(ImVec2(a.x + 5, a.y + 3), IM_COL32(0,0,0,200), label);
        dl->AddText(ImVec2(a.x + 4, a.y + 2), color, label);

        if (selected || hovered) {
            DrawHandles(dl, roi.rect, color);
        }
    }

    // Drawing-in-progress preview
    if (m_mode == Mode::Drawing &&
        m_drawingRect.width > 0 && m_drawingRect.height > 0) {
        const ImVec2 a = CanonToScreen(m_drawingRect.x, m_drawingRect.y);
        const ImVec2 b = CanonToScreen(
            m_drawingRect.x + m_drawingRect.width,
            m_drawingRect.y + m_drawingRect.height);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 40));
        dl->AddRect      (a, b, IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f);

        // Size readout near cursor
        char sz[64];
        std::snprintf(sz, sizeof(sz), "%dx%d  @ (%d,%d)",
                      m_drawingRect.width, m_drawingRect.height,
                      m_drawingRect.x, m_drawingRect.y);
        dl->AddText(ImVec2(mouseScreen.x + 12, mouseScreen.y + 12),
                    IM_COL32(0,0,0,200), sz);
        dl->AddText(ImVec2(mouseScreen.x + 11, mouseScreen.y + 11),
                    IM_COL32(255,255,255,255), sz);
    }

    dl->PopClipRect();
}

} // namespace fightsight
