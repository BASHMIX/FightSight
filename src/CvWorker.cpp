#include "App/CvWorker.h"

#include "App/FrameProcessor.h"
#include "App/RoiManager.h"
#include "App/WsClient.h"
#include "App/Util.h"

#include <algorithm>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

// Phase 11: Tesseract OCR. The detached worker function below is the
// ONLY place we touch the Tesseract API. Each call instantiates its own
// TessBaseAPI (Tesseract is NOT thread-safe across calls on a single
// instance), so multiple OCR fires can run concurrently without
// stepping on each other.
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>  // (header presence is enough; we
                                   //  feed Tesseract via SetImage from
                                   //  cv::Mat, not a Leptonica PIX)

namespace fightsight {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Monotonically-increasing request id - Streamer.bot silently drops
// DoAction frames that arrive without an id, and uses it for response
// correlation. Process-wide unique is sufficient.
std::atomic<uint64_t> g_triggerCounter{0};

// Build Streamer.bot DoAction JSON. Payload shape (Phase 6.5):
//   { "request": "DoAction",
//     "id":      "fightsight_trigger_<N>",
//     "action":  { "name": "<action>" },
//     "args":    { "state":      "triggered"|"cleared",
//                  "value":      <int|float>,
//                  "confidence": <float> } }
//
// args.value semantics:
//   Text:           integer state of the winning template variant
//   Gradient/Pixel: the percentage (float), same value used for threshold
// args.confidence is always the underlying float (matchTemplate score for
// Text, % for Gradient/Pixel) so the Streamer.bot script can still gate
// on raw confidence if it wants.
//
// Action resolution: roi.linked_action verbatim if set, else legacy
// "<SanitizeName(name)>_Triggered" / "..._Cleared".
std::string BuildDoActionJson(const Roi& roi,
                              const CvResult& result,
                              const char* state) {
    const uint64_t reqId = g_triggerCounter.fetch_add(1);

    std::string action;
    if (!roi.linked_action.empty()) {
        action = roi.linked_action;
    } else {
        const bool isTriggered = (std::strcmp(state, "triggered") == 0);
        action = SanitizeName(roi.name)
               + (isTriggered ? "_Triggered" : "_Cleared");
    }

    nlohmann::json j;
    j["request"]            = "DoAction";
    j["id"]                 = "fightsight_trigger_" + std::to_string(reqId);
    j["action"]["name"]     = action;
    j["args"]["state"]      = state;
    // Phase 9: result.value is on a unified 0-100 scale for ALL types.
    // For Text we still surface the winning integer state in args.value;
    // args.confidence carries the 0-100 percentage.
    if (roi.type == RoiType::Text) {
        j["args"]["value"]      = result.winningState; // integer
        j["args"]["confidence"] = result.value;        // float in [0,100]
    } else {
        j["args"]["value"]      = result.value;        // float in [0,100]
        j["args"]["confidence"] = result.value;        // same; kept for symmetry
    }
    return j.dump();
}

// ---------------------------------------------------------------------------
// Phase 11: detached OCR worker.
//
// Runs in a fire-and-forget thread spawned by the CV worker when a Text
// ROI fires with linked_ocr_roi_id set. The caller hands us:
//   bgrCrop    - an INDEPENDENT BGR cv::Mat (already .clone()'d, so we
//                won't see the source frame being recycled under us);
//   actionName - the Streamer.bot action name resolved at fire time
//                (linked_action, or "<SanitizeName(roi.name)>_Triggered"
//                if the user didn't bind one);
//   ws         - raw pointer to the long-lived WsClient. The CvWorker
//                holds a reference to the same instance and is always
//                stopped before the WsClient is destroyed, so the only
//                hazard is a detached OCR thread outliving the app's
//                main loop - which we accept as best-effort.
//
// Pipeline:
//   1. Grayscale
//   2. Otsu binary threshold (auto-picks the cutoff per crop)
//   3. Tesseract: LSTM-only, single-line PSM, digits/dash/space whitelist
//   4. Trim the UTF-8 result and ship a flat {event, score} payload.
void RunOcrAndSend(cv::Mat       bgrCrop,
                   std::string   actionName,
                   WsClient*     ws) {
    try {
        if (bgrCrop.empty() || !ws) return;

        // ---- 1) Grayscale ----
        cv::Mat gray;
        if (bgrCrop.channels() == 3) {
            cv::cvtColor(bgrCrop, gray, cv::COLOR_BGR2GRAY);
        } else if (bgrCrop.channels() == 1) {
            gray = bgrCrop;
        } else {
            return;
        }

        // ---- 2) Otsu binary threshold ----
        // THRESH_OTSU picks the cutoff automatically based on the
        // histogram of THIS crop - resilient to round-to-round
        // brightness changes without manual tuning.
        cv::Mat bin;
        cv::threshold(gray, bin, 0, 255,
                      cv::THRESH_BINARY | cv::THRESH_OTSU);

        // ---- 3) Tesseract ----
        tesseract::TessBaseAPI api;
        if (api.Init("./tessdata", "eng",
                     tesseract::OEM_LSTM_ONLY) != 0) {
            // Either tessdata/eng.traineddata is missing or the LSTM
            // model failed to load. Bail silently - nothing to ship.
            return;
        }
        api.SetPageSegMode(tesseract::PSM_SINGLE_LINE);
        api.SetVariable("tessedit_char_whitelist", "0123456789- ");

        // Feed the binarized image directly. step = number of bytes per
        // row (handles padded Mats correctly); 1 byte per pixel.
        api.SetImage(bin.data, bin.cols, bin.rows, 1,
                     static_cast<int>(bin.step));
        api.Recognize(nullptr);

        // GetUTF8Text returns a heap buffer the caller must release
        // with delete[] (Tesseract docs).
        char* raw = api.GetUTF8Text();
        std::string text;
        if (raw) {
            text.assign(raw);
            delete[] raw;
        }

        // ---- 4) Trim whitespace / newlines ----
        auto isWs = [](unsigned char c) {
            return std::isspace(c) != 0;
        };
        while (!text.empty() && isWs(text.front())) text.erase(text.begin());
        while (!text.empty() && isWs(text.back()))  text.pop_back();

        api.End();

        // ---- 5) WebSocket payload ----
        // Flat {event, score} shape (Phase 11 contract). Streamer.bot
        // side handles this separately from the standard DoAction
        // payload that BuildDoActionJson produces.
        if (!text.empty()) {
            nlohmann::json j;
            j["event"] = actionName;
            j["score"] = text;
            ws->Send(j.dump());
        }
    } catch (...) {
        // Best-effort: we have nowhere to surface errors from a
        // detached thread. Drop them on the floor.
    }
}

// Parse "<sanitized_name>_State_<int>.png" filenames in `dir`, returning
// (state, full_path) pairs. Silently skips non-matching files.
std::vector<std::pair<int, std::string>>
EnumerateStatePngs(const std::string& dir, const std::string& sanitizedName) {
    std::vector<std::pair<int, std::string>> out;
    const std::string prefix = sanitizedName + "_State_";
    const std::string ext    = ".png";
    try {
        if (!std::filesystem::exists(dir)) return out;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            const std::string fname = e.path().filename().string();
            if (fname.size() < prefix.size() + ext.size()) continue;
            if (fname.compare(0, prefix.size(), prefix) != 0) continue;
            if (fname.compare(fname.size() - ext.size(), ext.size(), ext) != 0)
                continue;
            const std::string mid =
                fname.substr(prefix.size(),
                             fname.size() - prefix.size() - ext.size());
            try {
                int n = std::stoi(mid);
                out.emplace_back(n, e.path().string());
            } catch (...) {}
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
CvWorker::CvWorker(FrameProcessor& fp, RoiManager& rm, WsClient& ws,
                   MatchManager& mm)
    : m_fp(fp), m_rm(rm), m_ws(ws), m_match(mm) {}

CvWorker::~CvWorker() { Stop(); }

void CvWorker::Start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread([this] { Run(); });
}

void CvWorker::Stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void CvWorker::SetTargetHz(int hz) {
    if (hz < 1)   hz = 1;
    if (hz > 240) hz = 240;
    m_targetHz.store(hz);
}

void CvWorker::InvalidateTemplate(int roiId) {
    std::lock_guard<std::mutex> lk(m_dirtyMutex);
    m_dirtyIds.insert(roiId);
}

void CvWorker::InvalidateAllTemplates() {
    std::lock_guard<std::mutex> lk(m_dirtyMutex);
    m_dirtyAll = true;
}

void CvWorker::ResetWatermark(int roiId) {
    std::lock_guard<std::mutex> lk(m_dirtyMutex);
    m_resetWatermarkIds.insert(roiId);
}

void CvWorker::SetTemplatesDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(m_dirtyMutex);
    if (m_templatesDir != dir) {
        m_templatesDir = dir;
        // Path change invalidates every cached template AND every runtime
        // bit (fired state + low-watermarks): we're now tracking a
        // different game's ROIs entirely.
        m_dirtyAll = true;
    }
}

std::string CvWorker::GetTemplatesDir() const {
    std::lock_guard<std::mutex> lk(m_dirtyMutex);
    return m_templatesDir;
}

std::unordered_map<int, CvResult> CvWorker::SnapshotResults() const {
    std::lock_guard<std::mutex> lk(m_resultsMutex);
    return m_results;
}

// ---------------------------------------------------------------------------
void CvWorker::Run() {
    using clock = std::chrono::steady_clock;
    auto fpsWindow = clock::now();
    int  framesInWindow = 0;

    // Wall-clock delta drives every cooldown_timer in the runtime
    // table. Initialized to now() so the very first cycle gets
    // delta_sec == 0 (no spurious tick).
    auto lastCycleTime = clock::now();

    while (m_running.load()) {
        const auto cycleStart = clock::now();
        const float delta_sec = std::chrono::duration<float>(
            cycleStart - lastCycleTime).count();
        lastCycleTime = cycleStart;

        // Process pending cache invalidations + snapshot templates dir.
        {
            std::lock_guard<std::mutex> lk(m_dirtyMutex);
            if (m_dirtyAll) {
                m_templateCache.clear();
                m_runtime.clear();   // also reset fired bits + watermarks
                m_dirtyAll = false;
            }
            for (int id : m_dirtyIds) m_templateCache.erase(id);
            m_dirtyIds.clear();
            // Manual watermark resets - clear committed + tentative lows
            // for the targeted ROIs.
            for (int id : m_resetWatermarkIds) {
                auto it = m_runtime.find(id);
                if (it != m_runtime.end()) {
                    it->second.lowest_value_seen  = 100.0f;
                    it->second.tentative_low      = 100.0f;
                    it->second.tentative_start_ms = 0;
                }
            }
            m_resetWatermarkIds.clear();
            m_cycleTemplatesDir = m_templatesDir;
        }

        cv::Mat frame;
        if (!m_fp.SnapshotProcessedFrame(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const bool sourceIsRgba = m_fp.IsSenderRGBA();

        const std::vector<Roi> rois = m_rm.SnapshotAll();

        // Garbage-collect runtime entries for ROIs that no longer exist.
        if (!m_runtime.empty()) {
            std::unordered_set<int> liveIds;
            liveIds.reserve(rois.size());
            for (const auto& r : rois) liveIds.insert(r.id);
            for (auto it = m_runtime.begin(); it != m_runtime.end();) {
                if (liveIds.find(it->first) == liveIds.end()) {
                    it = m_runtime.erase(it);
                } else {
                    ++it;
                }
            }
        }

        std::unordered_map<int, CvResult> results;
        results.reserve(rois.size());

        // ---- Phase 12: snapshot the MatchManager's allowed-groups set
        // for the CURRENT global state, once per cycle. When filtering
        // is disabled this stays empty and the per-ROI gate becomes a
        // no-op. Snapshotting up front means we hold the manager's
        // mutex only briefly even if its UI is being edited mid-cycle.
        const bool stateFilterOn = m_match.IsFilteringEnabled();
        const std::unordered_set<std::string> allowedGroups =
            stateFilterOn ? m_match.SnapshotAllowedGroups()
                          : std::unordered_set<std::string>{};

        // ---- Per-ROI pipeline (Phase 11b: merged for short-circuit) ----
        //   1. Tick the cooldown timer once per cycle.
        //   2. Short-circuit gate - if still in cooldown, SKIP the
        //      heavy CV (matchTemplate / inRange / mean-BGR /
        //      Otsu) entirely and publish the frozen prior reading.
        //      This is the optimization: locked-out ROIs cost ~nothing.
        //   3. Run ProcessOne (the heavy CV) only for ROIs that can
        //      actually fire this cycle.
        //   4. Drop-Only watermark filter (Gradient/Pixel only).
        //   5. EvaluateAndFire - the now-cooldown-free state machine.
        //   6. OCR launch on linked Text fires.
        //   7. MatchStart bookkeeping.
        //
        // Drop-Only's tentative-low buffer remains independent of the
        // post-fire cooldown - it uses a hard-coded 500ms grace so VFX
        // flashes don't poison the watermark, while roi.cooldown_duration
        // controls the post-fire WebSocket lockout.
        bool matchStartFiredThisCycle = false;
        for (const auto& roi : rois) {
            // ---- Phase 12: MatchManager group filter ----
            // The cheapest possible gate - O(1) set lookup against the
            // per-cycle snapshot. Skipped entirely when filtering is
            // disabled (allowedGroups is empty AND stateFilterOn is
            // false, so the check is short-circuited).
            if (stateFilterOn) {
                const std::string& g =
                    roi.group_name.empty() ? "Default" : roi.group_name;
                if (allowedGroups.find(g) == allowedGroups.end()) {
                    // Publish an Empty result so the UI can render
                    // "----" and the table row stays visible.
                    CvResult cr;
                    cr.status = CvResult::Status::Empty;
                    cr.note   = "filtered by match state";
                    results[roi.id] = std::move(cr);
                    continue;   // <<< skip all CV work
                }
            }

            // OcrZone is a passive bounding-box container - publish a
            // minimal Ok result and skip everything else (saves a
            // ProcessOne switch hit per ROI).
            if (roi.type == RoiType::OcrZone) {
                CvResult cr;
                cr.status = CvResult::Status::Ok;
                cr.value  = 0.0f;
                cr.note   = "ocr zone (no eval)";
                results[roi.id] = std::move(cr);
                continue;
            }

            RoiRuntime& rt = m_runtime[roi.id];

            // ---- 1) Tick cooldown FIRST so the gate below sees the
            //         latest value within this cycle.
            if (rt.cooldown_timer > 0.0f) {
                rt.cooldown_timer -= delta_sec;
                if (rt.cooldown_timer < 0.0f) rt.cooldown_timer = 0.0f;
            }

            // ---- 2) SHORT-CIRCUIT GATE ----
            // If we're still on cooldown, skip matchTemplate / inRange
            // / mean-BGR / Otsu / crop conversion - the ROI cannot fire
            // this cycle. Publish a frozen snapshot of the prior
            // reading so the table keeps showing context (the row will
            // get dimmed in the UI based on cooldown_remaining_ms).
            if (rt.cooldown_timer > 0.0f) {
                CvResult cr;
                cr.status                = CvResult::Status::Ok;
                cr.value                 = rt.lastValue;
                cr.winningState          = rt.last_sent_integer_state;
                cr.isFired               = rt.state;
                cr.cooldown_remaining_ms =
                    static_cast<int64_t>(rt.cooldown_timer * 1000.0f);
                cr.timestampMs           = NowMs();
                cr.note                  = "cooldown";
                results[roi.id] = std::move(cr);
                continue;   // <<< the whole point: skip the heavy CV
            }

            // ---- 3) Heavy CV ----
            CvResult cr;
            ProcessOne(roi, frame, sourceIsRgba, cr);

            // ---- 4) Drop-Only watermark with 500ms tentative grace --
            if (cr.status == CvResult::Status::Ok && roi.drop_only &&
                (roi.type == RoiType::Gradient || roi.type == RoiType::Pixel)) {
                constexpr int64_t kGraceMs = 500;
                const int64_t now = NowMs();
                if (cr.value >= rt.lowest_value_seen) {
                    rt.tentative_low      = rt.lowest_value_seen;
                    rt.tentative_start_ms = 0;
                } else {
                    if (rt.tentative_start_ms == 0
                        || cr.value < rt.tentative_low) {
                        rt.tentative_low      = cr.value;
                        rt.tentative_start_ms = now;
                    }
                    if (now - rt.tentative_start_ms >= kGraceMs) {
                        rt.lowest_value_seen  = rt.tentative_low;
                        rt.tentative_start_ms = 0;
                    }
                }
                cr.value = rt.lowest_value_seen;
            }

            // ---- 5) Edge-trigger evaluation ----
            // Cooldown is guaranteed 0 here (we'd have continued above
            // otherwise), so EvaluateAndFire's defensive gate is a
            // no-op and the state machine sees only valid fires.
            const bool fired = EvaluateAndFire(roi, cr);

            // ---- 6) OCR launch on linked Text fires (Phase 11) ----
            if (fired
                && roi.type == RoiType::Text
                && roi.linked_ocr_roi_id >= 0) {
                const Roi* zone = nullptr;
                for (const auto& other : rois) {
                    if (other.id == roi.linked_ocr_roi_id
                        && other.type == RoiType::OcrZone) {
                        zone = &other;
                        break;
                    }
                }
                if (zone) {
                    const cv::Rect safe = zone->rect
                        & cv::Rect(0, 0, frame.cols, frame.rows);
                    if (safe.width >= 4 && safe.height >= 4) {
                        // CRITICAL: .clone() to guarantee the OCR
                        // thread has an INDEPENDENT buffer.
                        cv::Mat cropClone = frame(safe).clone();
                        cv::Mat cropBgr;
                        if (sourceIsRgba) {
                            cv::cvtColor(cropClone, cropBgr,
                                         cv::COLOR_RGBA2BGR);
                        } else {
                            cv::cvtColor(cropClone, cropBgr,
                                         cv::COLOR_BGRA2BGR);
                        }

                        std::string actionName = roi.linked_action.empty()
                            ? SanitizeName(roi.name) + "_Triggered"
                            : roi.linked_action;

                        std::thread(RunOcrAndSend,
                                    std::move(cropBgr),
                                    std::move(actionName),
                                    &m_ws).detach();
                    }
                }
            }

            // ---- 7) MatchStart bookkeeping ----
            if (fired && roi.system_role == SystemRole::MatchStart) {
                matchStartFiredThisCycle = true;
            }

            // ---- 8) Phase 12: global state transition on fire ----
            // Atomic store; the NEXT cycle's allowedGroups snapshot
            // will see the new state. Intentionally takes effect after
            // this ROI's OCR launch + MatchStart book-keeping so the
            // outgoing state's side effects all run to completion.
            if (fired && roi.triggers_state_transition.has_value()) {
                m_match.SetCurrentState(*roi.triggers_state_transition);
            }

            results[roi.id] = std::move(cr);
        }

        if (matchStartFiredThisCycle) {
            for (auto& kv : m_runtime) {
                kv.second.lowest_value_seen = 100.0f;
            }
        }

        // ---- 3) Publish results -----------------------------------------
        {
            std::lock_guard<std::mutex> lk(m_resultsMutex);
            m_results = std::move(results);
        }

        // ---- 4) Stats + pacing ------------------------------------------
        const auto cycleEnd = clock::now();
        const double cycleMs = std::chrono::duration<double, std::milli>(
            cycleEnd - cycleStart).count();
        m_lastCycleMs.store(cycleMs);

        ++framesInWindow;
        const auto windowDur = cycleEnd - fpsWindow;
        if (windowDur >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(windowDur).count();
            m_actualHz.store(static_cast<double>(framesInWindow) / secs);
            framesInWindow = 0;
            fpsWindow = cycleEnd;
        }

        const int hz = m_targetHz.load();
        if (hz > 0) {
            const auto interval = std::chrono::microseconds(1000000 / hz);
            const auto wake = cycleStart + interval;
            if (clock::now() < wake) {
                std::this_thread::sleep_until(wake);
            }
            // else: cycle took longer than budget; loop immediately
        }
    }
}

// ---------------------------------------------------------------------------
void CvWorker::ProcessOne(const Roi& roi, const cv::Mat& frame,
                          bool sourceIsRgba, CvResult& out) {
    out.status = CvResult::Status::Error;
    out.value  = 0.0f;
    out.timestampMs = NowMs();
    out.note.clear();

    const cv::Rect safe = roi.rect & cv::Rect(0, 0, frame.cols, frame.rows);
    if (safe.width < 4 || safe.height < 4) {
        out.status = CvResult::Status::OutOfBounds;
        out.note   = "ROI does not fit frame";
        return;
    }

    cv::Mat crop = frame(safe);
    cv::Mat cropBgra;
    if (sourceIsRgba) {
        cv::cvtColor(crop, cropBgra, cv::COLOR_RGBA2BGRA);
    } else {
        // Already BGRA (the common Spout case) - keep as a view to avoid
        // an extra copy; downstream cvtColor will copy if needed.
        cropBgra = crop;
    }

    switch (roi.type) {
    // -----------------------------------------------------------------
    case RoiType::OcrZone: {
        // Pure bounding-box container - no CV evaluation, nothing to
        // publish. Mark Ok with value=0 so the row still appears in the
        // results map (the UI shows it as "idle" with value 0.00).
        out.status = CvResult::Status::Ok;
        out.value  = 0.0f;
        return;
    }
    // -----------------------------------------------------------------
    case RoiType::Text: {
        // Multi-state matching (Phase 6.5): load every
        // "<name>_State_<int>.png" variant, run matchTemplate against
        // each, pick the highest-confidence one. Legacy single-PNG
        // "<name>.png" is accepted as state 0 for backwards compat.
        CachedTemplateSet& set = m_templateCache[roi.id];
        if (!set.tried) {
            set.tried = true;
            const std::string sanitized = SanitizeName(roi.name);

            // Phase 10: split each PNG into a BGR template + (optional)
            // single-channel alpha mask. The mask gets fed straight to
            // cv::matchTemplate so transparent pixels in the artist's
            // template do not contribute to the correlation score - the
            // text/icon stays matchable across totally different
            // backgrounds (combo counters over varying stage art, etc.).
            //
            // Channel handling:
            //   4-ch (BGRA) -> extract A as mask, keep BGR for templ
            //   3-ch (BGR)  -> templ as-is, mask stays empty
            //   1-ch (gray) -> upcast to BGR, mask stays empty
            //   other       -> reject
            auto pushVariant = [&](int state, cv::Mat loaded) {
                if (loaded.empty()) return;

                CachedStateTemplate v;
                v.state = state;

                const int ch = loaded.channels();
                if (ch == 4) {
                    // Lift the alpha into a separate single-channel mask
                    // (matchTemplate's expected shape). extractChannel
                    // is a zero-copy split - cheaper than cv::split when
                    // we only need one channel.
                    cv::extractChannel(loaded, v.mask, 3);
                    cv::cvtColor(loaded, v.templ, cv::COLOR_BGRA2BGR);
                } else if (ch == 3) {
                    v.templ = loaded;
                    // v.mask intentionally left empty - full-image match.
                } else if (ch == 1) {
                    cv::cvtColor(loaded, v.templ, cv::COLOR_GRAY2BGR);
                } else {
                    return;
                }
                set.variants.push_back(std::move(v));
            };

            // Primary: enumerate "<name>_State_<N>.png" files in dir.
            // IMREAD_UNCHANGED preserves the 4th channel - critical;
            // IMREAD_COLOR would silently drop the alpha mask.
            for (const auto& [state, path] :
                 EnumerateStatePngs(m_cycleTemplatesDir, sanitized)) {
                pushVariant(state, cv::imread(path, cv::IMREAD_UNCHANGED));
            }

            // Legacy fallback: if no state-suffixed files exist, treat
            // "<name>.png" as state 0 so pre-Phase-6.5 configs still work.
            if (set.variants.empty()) {
                const std::string legacy =
                    m_cycleTemplatesDir + "/" + sanitized + ".png";
                pushVariant(0, cv::imread(legacy, cv::IMREAD_UNCHANGED));
            }

            if (set.variants.empty()) {
                set.note = "no templates";
            } else {
                std::sort(set.variants.begin(), set.variants.end(),
                    [](const auto& a, const auto& b) {
                        return a.state < b.state;
                    });
            }
        }

        if (set.variants.empty()) {
            out.status = CvResult::Status::NoTemplate;
            out.note   = set.note.empty() ? "no templates" : set.note;
            return;
        }

        // Convert the ROI crop to BGR once before the per-variant loop.
        // matchTemplate wants the search image, the template, and the
        // mask to all live in the same colour space; doing the cvtColor
        // here avoids redoing it for every state variant.
        cv::Mat cropBgr;
        cv::cvtColor(cropBgra, cropBgr, cv::COLOR_BGRA2BGR);

        // Score every variant; keep the best. Each variant gets its own
        // resize since templates may be different sizes (e.g. wider Combo
        // values).
        float bestConf  = -1.0f;
        int   bestState = set.variants.front().state;

        for (const auto& v : set.variants) {
            cv::Mat searchArea;
            if (cropBgr.size() != v.templ.size()) {
                cv::resize(cropBgr, searchArea, v.templ.size(),
                           0.0, 0.0, cv::INTER_LINEAR);
            } else {
                searchArea = cropBgr;
            }
            cv::Mat result;
            try {
                // Phase 10: pass the alpha mask when one was lifted out
                // of the PNG so transparent pixels do not influence the
                // correlation. matchTemplate's mask parameter is fully
                // supported by TM_CCORR_NORMED on every OpenCV version
                // we target (3.4+); TM_CCOEFF_NORMED only gained mask
                // support in 3.4.1+, so sticking to TM_CCORR_NORMED is
                // the portable choice when a mask is present. We pass
                // cv::noArray() when v.mask is empty so non-alpha PNGs
                // take the unmasked fast path.
                if (v.mask.empty()) {
                    cv::matchTemplate(searchArea, v.templ, result,
                                      cv::TM_CCORR_NORMED);
                } else {
                    cv::matchTemplate(searchArea, v.templ, result,
                                      cv::TM_CCORR_NORMED, v.mask);
                }
                if (result.empty()) continue;
                float c = result.at<float>(0, 0);
                if (!std::isfinite(c)) continue;
                // Phase 9: normalize Text confidence to 0-100 so the
                // threshold/value UX matches Gradient/Pixel. Correlation
                // is in [0,1] from TM_CCORR_NORMED; clamp first, then
                // scale, then keep the highest scorer across variants.
                c = std::max(0.0f, std::min(1.0f, c)) * 100.0f;
                if (c > bestConf) {
                    bestConf  = c;
                    bestState = v.state;
                }
            } catch (...) { /* skip variant */ }
        }

        if (bestConf < 0.0f) {
            out.note = "matchTemplate failed for all variants";
            return;
        }
        out.value         = bestConf;
        out.winningState  = bestState;
        out.status        = CvResult::Status::Ok;
        break;
    }
    // -----------------------------------------------------------------
    case RoiType::Gradient: {
        // HSV-range mask, % of matching pixels. Bounds are derived from
        // the designer-friendly base + tolerances model. When ignore_color
        // is on we collapse H/S to the full range, making inRange behave
        // as a pure luminance threshold on the V channel - exactly what
        // SF6 health bars need (red P1 -> blue P2 -> yellow low ->
        // grey regen all stay "bright" relative to the empty bar).
        try {
            cv::Mat bgr;
            cv::cvtColor(cropBgra, bgr, cv::COLOR_BGRA2BGR);
            cv::Mat hsv;
            cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

            int hLo, hHi, sLo, sHi, vLo, vHi;
            if (roi.ignore_color) {
                hLo = 0;   hHi = 179;
                sLo = 0;   sHi = 255;
            } else {
                hLo = std::max(0,   roi.base_hsv[0] - roi.hue_tolerance);
                hHi = std::min(179, roi.base_hsv[0] + roi.hue_tolerance);
                sLo = std::max(0,   roi.base_hsv[1] - roi.sat_tolerance);
                sHi = std::min(255, roi.base_hsv[1] + roi.sat_tolerance);
            }
            vLo = std::max(0,   roi.base_hsv[2] - roi.val_tolerance);
            vHi = std::min(255, roi.base_hsv[2] + roi.val_tolerance);

            cv::Mat mask;
            cv::inRange(hsv,
                cv::Scalar(hLo, sLo, vLo),
                cv::Scalar(hHi, sHi, vHi),
                mask);

            const int nonZero = cv::countNonZero(mask);
            const int total   = mask.rows * mask.cols;
            out.value = (total > 0)
                ? (100.0f * static_cast<float>(nonZero) / static_cast<float>(total))
                : 0.0f;

            // Aux = mean H,S,V of crop, for the tuner popup.
            const cv::Scalar meanHsv = cv::mean(hsv);
            out.aux[0] = static_cast<int>(meanHsv[0]);
            out.aux[1] = static_cast<int>(meanHsv[1]);
            out.aux[2] = static_cast<int>(meanHsv[2]);
            out.status = CvResult::Status::Ok;
        } catch (const std::exception& e) {
            out.note = e.what();
        }
        break;
    }
    // -----------------------------------------------------------------
    case RoiType::Pixel: {
        // Mean BGR of the crop vs target colour. Similarity is reported
        // as a percentage where 100 = identical and 0 = maximum distance
        // (white vs black).
        try {
            cv::Mat bgr;
            cv::cvtColor(cropBgra, bgr, cv::COLOR_BGRA2BGR);
            const cv::Scalar mean = cv::mean(bgr);

            const float db = static_cast<float>(mean[0]) - roi.target_bgr[0];
            const float dg = static_cast<float>(mean[1]) - roi.target_bgr[1];
            const float dr = static_cast<float>(mean[2]) - roi.target_bgr[2];
            const float dist = std::sqrt(db*db + dg*dg + dr*dr);

            constexpr float kMaxDist = 441.6729559300637f; // sqrt(255*255*3)
            const float similarity = std::max(
                0.0f, 100.0f * (1.0f - dist / kMaxDist));
            out.value = similarity;

            out.aux[0] = static_cast<int>(mean[0]); // B
            out.aux[1] = static_cast<int>(mean[1]); // G
            out.aux[2] = static_cast<int>(mean[2]); // R
            out.status = CvResult::Status::Ok;
        } catch (const std::exception& e) {
            out.note = e.what();
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Phase 11b: strict edge-trigger state machine, post short-circuit.
//
// The cooldown TICK and the heavy-CV short-circuit GATE now both live
// in CvWorker::Run() so we can skip matchTemplate / inRange entirely
// while a ROI is locked out. By the time this function is called,
// rt.cooldown_timer has already been ticked for the current cycle and
// is guaranteed to be 0 in the normal flow.
//
// Step order:
//   1. CV-error early-out: preserve prior state, no side effects.
//   2. Defensive cooldown gate. In the normal Run() flow this branch
//      is unreachable (caller skipped ProcessOne), but kept as a
//      belt-and-braces safety net so future callers can't accidentally
//      double-fire.
//   3. Type-specific edge evaluation:
//        Text     -> fire only when winning integer state differs
//                    from last_sent_integer_state, AND confidence
//                    cleared the threshold.
//        Gradient / Pixel -> fire only on rising edge.
//      A fire sets cooldown_timer = roi.cooldown_duration and queues
//      exactly ONE WebSocket payload (suppressed for Text+OCR linkage).
//   4. Falling edge / loss-of-confidence drops state back to IDLE but
//      does NOT send a payload and does NOT touch the cooldown timer.
bool CvWorker::EvaluateAndFire(const Roi& roi, CvResult& cr) {
    // OcrZone is filtered out by the caller now, but keep the guard
    // for safety if a future caller path forgets.
    if (roi.type == RoiType::OcrZone) {
        cr.isFired = false;
        cr.cooldown_remaining_ms = 0;
        return false;
    }

    RoiRuntime& rt = m_runtime[roi.id];
    rt.lastValue = cr.value;
    cr.cooldown_remaining_ms =
        static_cast<int64_t>(rt.cooldown_timer * 1000.0f);

    // ---- 1) CV-error early-out (preserve state, no side effects) ----
    if (cr.status != CvResult::Status::Ok) {
        cr.isFired = rt.state;
        return false;
    }

    // ---- 2) Defensive cooldown gate (caller normally short-circuits) ----
    if (rt.cooldown_timer > 0.0f) {
        cr.isFired = rt.state;
        return false;
    }

    bool firedThisCycle = false;

    // ---- 3) Type-specific edge evaluation ----
    if (roi.type == RoiType::Text) {
        // Discrete state-change lock. We still require the match to
        // clear the threshold so low-confidence noise doesn't trigger
        // a state change on garbage data.
        const bool confident = (cr.value >= roi.threshold);
        if (confident
            && cr.winningState != rt.last_sent_integer_state) {
            rt.last_sent_integer_state = cr.winningState;
            rt.cooldown_timer          = roi.cooldown_duration;
            rt.state                   = true;
            // Phase 11: when an OCR zone is linked, defer the WS send
            // to the detached OCR thread spawned by the caller (which
            // ships a flat {event, score} payload instead of the
            // standard DoAction). Without a link we keep the legacy
            // DoAction path so existing Streamer.bot scripts keep
            // working unchanged.
            if (roi.linked_ocr_roi_id < 0) {
                m_ws.Send(BuildDoActionJson(roi, cr, "triggered"));
            }
            firedThisCycle = true;
        } else if (!confident) {
            // Lost the match (text left the screen / faded out). Drop
            // the FIRED latch AND clear the discrete-state memory so
            // the NEXT appearance can fire fresh - critical for
            // single-event Text ROIs like "FIGHT!" where the same
            // integer state reappears every round.
            //
            // Spam protection during a single appearance is still
            // handled by cooldown_timer above: if confidence briefly
            // dips and bounces back within the cooldown window, the
            // absolute cooldown gate at the top of EvaluateAndFire
            // blocks the re-fire even though last_sent has been
            // cleared here. Belt and braces.
            rt.state                   = false;
            rt.last_sent_integer_state = -999;
        }
    } else {
        // Continuous rising-edge lock (Gradient / Pixel).
        const bool condition = roi.fire_when_above
            ? (cr.value > roi.threshold)
            : (cr.value < roi.threshold);

        if (condition && !rt.state) {
            rt.state          = true;
            rt.cooldown_timer = roi.cooldown_duration;
            m_ws.Send(BuildDoActionJson(roi, cr, "triggered"));
            firedThisCycle = true;
        } else if (!condition) {
            // ---- 4) Release the FIRED latch on falling edge.
            // No "cleared" payload by design - one payload per fire.
            rt.state = false;
        }
    }

    // Re-publish cooldown + fired state after any mutation above.
    cr.cooldown_remaining_ms =
        static_cast<int64_t>(rt.cooldown_timer * 1000.0f);
    cr.isFired = rt.state;
    return firedThisCycle;
}

} // namespace fightsight
