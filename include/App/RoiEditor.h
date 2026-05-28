#pragma once

#include "App/RoiManager.h"
#include <imgui.h>
#include <opencv2/core.hpp>

namespace fightsight {

// Interactive ROI editor overlay. Runs INSIDE the Spout Viewer window,
// immediately after the ImGui::Image() call that drew the video frame.
//
// All ROI data lives in canonical pixel coordinates (canonW x canonH).
// Screen-space numbers only exist inside this class for the duration of
// a single input frame.
class RoiEditor {
public:
    explicit RoiEditor(RoiManager& mgr) : m_mgr(mgr) {}

    // Call exactly once per frame, INSIDE the Spout Viewer Begin/End and
    // immediately after ImGui::Image(). Caller passes:
    //   imgScreenMin/Max - the on-screen rect the video is drawn at
    //                      (from ImGui::GetItemRectMin/Max right after Image)
    //   canonW, canonH   - the canonical resolution ROIs are stored in
    //                      (= processor.GetDisplayWidth/Height)
    // zoomLevel: 1.0 = full image visible (no zoom)
    // panUV:     top-left of visible region in normalized [0,1] canon space
    //            (only meaningful when zoomLevel > 1.0)
    void DrawAndHandleInput(const ImVec2& imgScreenMin,
                            const ImVec2& imgScreenMax,
                            int canonW, int canonH,
                            float zoomLevel = 1.0f,
                            const ImVec2& panUV = ImVec2(0.0f, 0.0f));

    int  SelectedId() const  { return m_selectedId; }
    void SetSelected(int id) { m_selectedId = id; }
    void ClearSelection()    { m_selectedId = 0;  }

    // Type used for the next user-drawn ROI.
    RoiType DefaultType() const     { return m_defaultType; }
    void    SetDefaultType(RoiType t) { m_defaultType = t; }

private:
    enum class Mode {
        Idle,
        Drawing,         // rubber-band a new rect from anchor
        Moving,          // body drag
        ResizingHandle,  // corner or edge drag
    };

    // 8 grab handles + body.
    enum class Handle : int {
        None        = -1,
        TopLeft     = 0,
        Top         = 1,
        TopRight    = 2,
        Right       = 3,
        BottomRight = 4,
        Bottom      = 5,
        BottomLeft  = 6,
        Left        = 7,
        Body        = 8,
    };

    // --- Coord transforms (cached per-frame from DrawAndHandleInput args) ---
    cv::Point ScreenToCanon(const ImVec2& s) const;
    ImVec2    CanonToScreen(int x, int y)   const;

    // Hit test against one rect. Returns the most-specific handle hit, or None.
    // tolCanon is the grab slack in canonical pixels (=8 screen px scaled).
    static Handle HitTestRoi(const cv::Rect& r,
                             const cv::Point& pt,
                             float tolCanon);

    // Visual cursor hint based on handle.
    static ImGuiMouseCursor CursorForHandle(Handle h);

    // Color from ROI type.
    static ImU32 ColorForType(RoiType t);

    void DrawHandles(ImDrawList* dl, const cv::Rect& r, ImU32 color);
    void ApplyHandleDrag(cv::Rect& out,
                         const cv::Rect& original,
                         const cv::Point& delta,
                         Handle handle,
                         int canonW, int canonH);

    RoiManager& m_mgr;
    RoiType     m_defaultType  = RoiType::Text;

    // Selection + hover state (computed every frame)
    int    m_selectedId   = 0; // 0 = none
    int    m_hoveredId    = 0;
    Handle m_hoveredHandle = Handle::None;

    // Active drag state
    Mode      m_mode             = Mode::Idle;
    Handle    m_dragHandle       = Handle::None;
    cv::Point m_dragAnchor;            // mouse-down canon position
    cv::Rect  m_dragOriginalRect;      // for move/resize
    cv::Rect  m_drawingRect;           // for new-rect drag preview

    // Cached transform state for the current frame.
    ImVec2 m_imgMin{};
    ImVec2 m_imgMax{};
    int    m_canonW = 1920;
    int    m_canonH = 1080;
    float  m_zoom   = 1.0f;
    ImVec2 m_panUV{0.0f, 0.0f};
};

} // namespace fightsight
