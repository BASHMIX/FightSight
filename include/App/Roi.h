#pragma once

#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "App/MatchManager.h"

namespace fightsight {

// Trigger type - how this ROI's pixels will be interpreted in the CV worker.
// The values are part of the on-disk config schema; don't renumber.
//   OcrZone is a passive bounding-box container - the CV worker skips it
//   entirely. A Text ROI references one via linked_ocr_roi_id; when the
//   Text ROI fires we crop the OcrZone's rect from the current frame and
//   hand the clone to a detached Tesseract thread.
enum class RoiType : int {
    Text     = 0, // template match w/ alpha mask
    Gradient = 1, // HSV inRange mask, % of matching pixels (0-100)
    Pixel    = 2, // mean BGR distance to target color, similarity % (0-100)
    OcrZone  = 3, // passive container - no CV eval, used by linked Text ROIs
};

// Game-state role this ROI plays in the CV pipeline. Most ROIs are `None`
// and just fire their action on edge transitions. SystemRole adds extra
// behavior on top:
//   MatchStart      - rising edge resets ALL Drop-Only low-watermarks
//                     in the current profile (use for "FIGHT!" detection,
//                     versus-screen disappearance, etc.)
//   RoundTransition - reserved; currently no built-in effect, but the
//                     edge still fires its DoAction so user scripts can
//                     react.
enum class SystemRole : int {
    None            = 0,
    MatchStart      = 1,
    RoundTransition = 2,
};

inline const char* SystemRoleName(SystemRole r) {
    switch (r) {
        case SystemRole::None:            return "None";
        case SystemRole::MatchStart:      return "MatchStart";
        case SystemRole::RoundTransition: return "RoundTransition";
    }
    return "None";
}

inline SystemRole SystemRoleFromName(const std::string& s) {
    if (s == "MatchStart")      return SystemRole::MatchStart;
    if (s == "RoundTransition") return SystemRole::RoundTransition;
    return SystemRole::None;
}

inline const char* RoiTypeName(RoiType t) {
    switch (t) {
        case RoiType::Text:     return "Text";
        case RoiType::Gradient: return "Gradient";
        case RoiType::Pixel:    return "Pixel";
        case RoiType::OcrZone:  return "OcrZone";
    }
    return "?";
}

inline RoiType RoiTypeFromName(const std::string& s) {
    if (s == "Gradient") return RoiType::Gradient;
    if (s == "Pixel")    return RoiType::Pixel;
    if (s == "OcrZone")  return RoiType::OcrZone;
    return RoiType::Text;
}

// Sensible threshold default per type. Phase 9 unifies the UX so ALL types
// use a [0,100] scale - Text now multiplies its matchTemplate correlation
// by 100 in CvWorker, so 0.95 confidence reads as 95.0 in the table.
inline float DefaultThresholdFor(RoiType t) {
    switch (t) {
        case RoiType::Text:     return 85.0f;
        case RoiType::Gradient: return 50.0f;
        case RoiType::Pixel:    return 80.0f;
        case RoiType::OcrZone:  return 0.0f;   // unused; container only
    }
    return 50.0f;
}

// All rect coordinates are in the **canonical processor display space**,
// which is 1920x1080 when Force-Internal-1080p is enabled (the recommended
// editing mode). Never store screen-space coordinates.
struct Roi {
    int         id   = 0;
    // The display label AND the Streamer.bot action name AND the template
    // PNG filename (sanitized). When the WebSocket is connected and the
    // Action Combo is visible in the table, this is picked from the live
    // list fetched from Streamer.bot.
    std::string name;
    RoiType     type = RoiType::Text;
    cv::Rect    rect;

    // Trigger config (Phase 9 - unified 0-100 scale across all types):
    //   Text:     threshold = confidence percentage in [0,100]
    //   Gradient: threshold = % of HSV-matching pixels in [0,100]
    //   Pixel:    threshold = % BGR similarity in [0,100]
    float       threshold       = 85.0f;
    bool        fire_when_above = true;

    // Drop-Only "tentative low" grace window in seconds. A candidate new
    // low must persist for this long before being committed to the
    // watermark - guards against single-frame VFX flashes spiking a value
    // back to 100% mid-round. Only meaningful when drop_only = true and
    // type is Gradient or Pixel. Default 0.5s matches Phase 8 behavior.
    float       cooldown_duration = 0.5f;

    // ---- Gradient: designer-friendly HSV picker ----
    // CvWorker derives the cv::inRange (lower, upper) bounds at evaluation
    // time:
    //   lower = base_hsv - {hue_tol, sat_tol, val_tol}   (clamped)
    //   upper = base_hsv + {hue_tol, sat_tol, val_tol}   (clamped)
    // When ignore_color is true, H is forced to [0,179] and S to [0,255]
    // so inRange becomes a pure V-channel (luminance) threshold - great
    // for SF6 health bars that change color (red/blue/yellow/grey).
    int  base_hsv[3]   = {0, 200, 200}; // vivid red default (P1 health)
    int  hue_tolerance = 10;            // 0..90
    int  sat_tolerance = 60;            // 0..128
    int  val_tolerance = 60;            // 0..128
    bool ignore_color  = false;

    // ---- Pixel: target colour for mean-BGR similarity ----
    int target_bgr[3] = {255, 255, 255};

    // ---- Phase 6: game-state context & VFX occlusion handling ----
    // Drop-Only ("low-watermark") locks the published Value to its lowest
    // seen reading until a MatchStart ROI fires - prevents super-move VFX
    // flashes from spiking a health bar back to 100% mid-round.
    // Only meaningful for Gradient/Pixel (continuous values); ignored for
    // Text (discrete confidence).
    bool       drop_only   = false;
    SystemRole system_role = SystemRole::None;

    // ---- Phase 7: Streamer.bot action binding + UX helpers ----
    // Explicit Streamer.bot action this ROI fires (picked from the live
    // catalogue in the table). Empty = legacy fallback: send
    //   "<SanitizeName(name)>_Triggered" / "..._Cleared"
    // on the two edges, so configs without an explicit binding behave
    // exactly as Phase 4 did.
    std::string linked_action;

    // Render-time overlay: when true, the captured PNG is alpha-blended
    // at this ROI's rect over the live feed (Text type only).
    bool        ghost_overlay = false;

    // ---- Phase 11: OCR linkage ----
    // When a Text ROI fires (rising edge / state change, past cooldown)
    // and this is set to the id of an OcrZone ROI, the CV worker crops
    // the OcrZone's rect from the live frame and hands the clone to a
    // detached Tesseract thread. The result is published over the
    // WebSocket as a separate score payload. -1 = no OCR linkage.
    int         linked_ocr_roi_id = -1;

    // ---- Phase 12: Match-state transition trigger ----
    // When set, a successful fire on this ROI atomically switches the
    // global MatchManager::current_state to the target state. The next
    // CV cycle's group-filter pass uses the new state's allowed-groups
    // set. nullopt = this ROI does not affect global state.
    std::optional<MatchState> triggers_state_transition;

    // ---- Phase 8: workflow polish ----
    // Collapsible group in the ROIs table (e.g. "P1 HUD", "P2 HUD",
    // "Match State"). Empty/default falls back to "Default".
    std::string group_name = "Default";
    // Row-background tint for the ROIs table (RGBA in [0,1]). Default
    // is a very dark grey that sits a hair above the dark-theme
    // WindowBg, so the ROIs table reads as part of the surrounding
    // panel chrome and white text stays cleanly legible. The Inspector
    // exposes an AlphaBar for users who want a brighter accent.
    float       row_color[4] = {0.15f, 0.15f, 0.15f, 1.0f};

    nlohmann::json ToJson() const {
        nlohmann::json j = {
            {"id",   id},
            {"name", name},
            {"type", RoiTypeName(type)},
            {"x", rect.x}, {"y", rect.y},
            {"w", rect.width}, {"h", rect.height},
            {"threshold",         threshold},
            {"fire_when_above",   fire_when_above},
            {"cooldown_duration", cooldown_duration},

            {"base_hsv",       {base_hsv[0],   base_hsv[1],   base_hsv[2]}},
            {"hue_tolerance",   hue_tolerance},
            {"sat_tolerance",   sat_tolerance},
            {"val_tolerance",   val_tolerance},
            {"ignore_color",    ignore_color},

            {"target_bgr",     {target_bgr[0], target_bgr[1], target_bgr[2]}},

            {"drop_only",      drop_only},
            {"system_role",    SystemRoleName(system_role)},

            {"linked_action",     linked_action},
            {"ghost_overlay",     ghost_overlay},
            {"linked_ocr_roi_id", linked_ocr_roi_id},

            {"group_name", group_name},
            {"row_color",  {row_color[0], row_color[1],
                            row_color[2], row_color[3]}},
        };
        // optional<MatchState> - only emit when set, so older readers
        // simply see the field as absent.
        if (triggers_state_transition.has_value()) {
            j["triggers_state_transition"] =
                MatchStateName(*triggers_state_transition);
        }
        return j;
    }

    static Roi FromJson(const nlohmann::json& j) {
        Roi r;
        r.id   = j.value("id",   0);
        r.name = j.value("name", std::string{});
        r.type = RoiTypeFromName(j.value("type", std::string{"Text"}));
        r.rect = cv::Rect(j.value("x", 0), j.value("y", 0),
                          j.value("w", 0), j.value("h", 0));
        r.threshold         = j.value("threshold",         DefaultThresholdFor(r.type));
        r.fire_when_above   = j.value("fire_when_above",   true);
        r.cooldown_duration = j.value("cooldown_duration", 0.5f);

        // Migration safety net 1: Gradient/Pixel use [0,100] units, but old
        // configs created before the unit-fix may carry a 0.85 default from
        // the Text era. Bump to a sane value to prevent always-fire.
        if ((r.type == RoiType::Gradient || r.type == RoiType::Pixel)
            && r.threshold > 0.0f && r.threshold < 1.0f) {
            r.threshold = DefaultThresholdFor(r.type);
        }
        // Migration safety net 2 (Phase 9): Text ROIs are now scored on a
        // 0-100 scale to match Gradient/Pixel. Configs from earlier
        // versions stored thresholds in [0,1]; rescale them so legacy
        // profiles keep working without a manual edit.
        if (r.type == RoiType::Text && r.threshold > 0.0f && r.threshold <= 1.0f) {
            r.threshold *= 100.0f;
        }

        auto loadTriple = [](const nlohmann::json& arr,
                             int dst[3], int d0, int d1, int d2) {
            if (arr.is_array() && arr.size() == 3) {
                dst[0] = arr[0].get<int>();
                dst[1] = arr[1].get<int>();
                dst[2] = arr[2].get<int>();
            } else {
                dst[0] = d0; dst[1] = d1; dst[2] = d2;
            }
        };

        // ---- Gradient: prefer new fields; fall back to inferring from
        // legacy hsv_min/hsv_max if present. ----
        if (j.contains("base_hsv")) {
            loadTriple(j["base_hsv"], r.base_hsv, 0, 200, 200);
            r.hue_tolerance = j.value("hue_tolerance", 10);
            r.sat_tolerance = j.value("sat_tolerance", 60);
            r.val_tolerance = j.value("val_tolerance", 60);
            r.ignore_color  = j.value("ignore_color",  false);
        } else if (j.contains("hsv_min") && j.contains("hsv_max")
                   && j["hsv_min"].is_array() && j["hsv_max"].is_array()
                   && j["hsv_min"].size() == 3 && j["hsv_max"].size() == 3) {
            const int lo[3] = {
                j["hsv_min"][0].get<int>(),
                j["hsv_min"][1].get<int>(),
                j["hsv_min"][2].get<int>(),
            };
            const int hi[3] = {
                j["hsv_max"][0].get<int>(),
                j["hsv_max"][1].get<int>(),
                j["hsv_max"][2].get<int>(),
            };
            for (int i = 0; i < 3; ++i) {
                r.base_hsv[i]  = (lo[i] + hi[i]) / 2;
            }
            r.hue_tolerance = (hi[0] - lo[0]) / 2;
            r.sat_tolerance = (hi[1] - lo[1]) / 2;
            r.val_tolerance = (hi[2] - lo[2]) / 2;
            r.ignore_color  = false;
        }

        // ---- Pixel target ----
        if (j.contains("target_bgr"))
            loadTriple(j["target_bgr"], r.target_bgr, 255, 255, 255);

        // ---- Game-state context ----
        r.drop_only   = j.value("drop_only", false);
        r.system_role = SystemRoleFromName(
            j.value("system_role", std::string{"None"}));

        // ---- Action binding + UX helpers ----
        r.linked_action     = j.value("linked_action",     std::string{});
        r.ghost_overlay     = j.value("ghost_overlay",     false);
        r.linked_ocr_roi_id = j.value("linked_ocr_roi_id", -1);

        // ---- Phase 12: optional global-state transition trigger ----
        if (j.contains("triggers_state_transition")
            && j["triggers_state_transition"].is_string()) {
            r.triggers_state_transition = MatchStateFromName(
                j["triggers_state_transition"].get<std::string>());
        }

        // ---- Grouping + row colour ----
        r.group_name = j.value("group_name", std::string{"Default"});
        if (r.group_name.empty()) r.group_name = "Default";
        if (j.contains("row_color") && j["row_color"].is_array()
            && j["row_color"].size() == 4) {
            for (int i = 0; i < 4; ++i)
                r.row_color[i] = j["row_color"][i].get<float>();
        }

        return r;
    }
};

} // namespace fightsight
