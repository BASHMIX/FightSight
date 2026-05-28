#pragma once

#include "App/Roi.h"
#include "App/MatchManager.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <opencv2/core.hpp>

namespace fightsight {

class FrameProcessor;
class RoiManager;
class WsClient;

// Per-ROI CV result published to the render thread (read-only on that side).
struct CvResult {
    enum class Status : int {
        Empty       = 0, // never processed
        NoTemplate  = 1, // Text ROI but PNG missing or unreadable
        OutOfBounds = 2, // rect doesn't fit current frame
        Ok          = 3,
        Error       = 4,
    };
    Status   status      = Status::Empty;
    float    value       = 0.0f;   // Text: best-state confidence [0,1]; Gradient/Pixel: % [0,100]
    int      winningState = 0;     // Text: integer state of the highest-confidence template variant
    bool     isFired     = false;
    int64_t  timestampMs = 0;
    // ms remaining in the Drop-Only grace period (the "tentative low"
    // buffer). 0 when Drop-Only is off or no candidate low is pending.
    int64_t  cooldown_remaining_ms = 0;
    std::string note;              // human-readable diag when not Ok

    // Per-type tuning aids. CvWorker populates these so the per-ROI Cfg
    // popup can show live values without doing its own CV pass.
    //   Gradient -> aux = mean H/S/V of crop
    //   Pixel    -> aux = mean B/G/R of crop
    //   Text     -> unused
    int aux[3] = {0, 0, 0};
};

// Dedicated CV thread. Snapshots the latest frame + ROI list each cycle,
// runs matchTemplate / HSV ops, publishes results, and edge-detects state
// transitions to push Streamer.bot DoAction JSON via WsClient.
class CvWorker {
public:
    CvWorker(FrameProcessor& fp, RoiManager& rm, WsClient& ws,
             MatchManager& mm);
    ~CvWorker();

    CvWorker(const CvWorker&) = delete;
    CvWorker& operator=(const CvWorker&) = delete;

    void Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    void SetTargetHz(int hz);
    int  GetTargetHz()   const { return m_targetHz.load();   }
    double GetActualHz() const { return m_actualHz.load();   }
    double GetLastCycleMs() const { return m_lastCycleMs.load(); }

    // Thread-safe; the CV thread snapshots the current value at the start
    // of each cycle into m_cycleTemplatesDir.
    void        SetTemplatesDir(const std::string& dir);
    std::string GetTemplatesDir() const;

    // Force the CV thread to drop its cached template for the given ROI -
    // call after a successful Capture so the next cycle reloads from disk.
    void InvalidateTemplate(int roiId);

    // Drop every cached template AND clear all runtime state (fired bits,
    // drop-only low-watermarks). Use on profile switch.
    void InvalidateAllTemplates();

    // Manually reset the Drop-Only low-watermark for one ROI back to
    // 100.0 (and cancel any pending tentative low). Effective on the
    // next CV cycle.
    void ResetWatermark(int roiId);

    // Render-thread accessor. Locks briefly, returns a copy.
    std::unordered_map<int, CvResult> SnapshotResults() const;

private:
    void Run();
    void ProcessOne(const Roi& roi, const cv::Mat& frame,
                    bool sourceIsRgba, CvResult& out);
    // Returns true if this evaluation produced a "triggered" send
    // (rising edge for continuous, state-change fire for Text). The
    // caller uses the bool to detect MatchStart system-role fires.
    //
    // Phase 11b: the cooldown TICK and the short-circuit GATE both
    // live in the Run() loop now (so we can skip the heavy CV when in
    // cooldown). This method assumes the caller has already ticked
    // rt.cooldown_timer for the current cycle; the defensive gate
    // inside is a belt-and-braces safety net.
    bool EvaluateAndFire(const Roi& roi, CvResult& cr);

    // One PNG variant for an integer state.
    //   templ: BGR, CV_8UC3 - the colour channels of the template.
    //   mask:  CV_8UC1 alpha lifted from the source PNG, OR empty.
    //          When non-empty it's passed as the matchTemplate mask so
    //          transparent template pixels do not influence the score
    //          (the Phase 10 background-immunity trick). Empty means
    //          "PNG had no alpha channel; score every pixel equally."
    struct CachedStateTemplate {
        int     state = 0;
        cv::Mat templ;  // BGR, CV_8UC3
        cv::Mat mask;   // CV_8UC1 (empty if source had no alpha)
    };

    // Full set of variants for one ROI. Loaded lazily on first CV pass.
    struct CachedTemplateSet {
        std::vector<CachedStateTemplate> variants;
        bool        tried = false;
        std::string note;
    };

    // Phase 10b: strict edge-trigger state machine, single-threaded.
    //   state                   - false = IDLE (eligible to fire),
    //                             true  = FIRED (latched until the
    //                             trigger condition releases).
    //   cooldown_timer          - seconds remaining in the post-fire
    //                             lockout. Decremented each cycle by
    //                             the actual wall-clock delta so the
    //                             lockout matches roi.cooldown_duration
    //                             regardless of target_hz drift.
    //   last_sent_integer_state - Text only. The winning integer state
    //                             of the most recent fire we sent to
    //                             Streamer.bot. -999 sentinel so the
    //                             first valid detection counts as a
    //                             state change.
    //
    // Drop-Only watermark machinery is now fully independent of the
    // post-fire cooldown above (it uses its own hard-coded 500ms
    // tentative-low buffer so VFX flashes can't poison the low).
    struct RoiRuntime {
        bool  state                   = false;
        float cooldown_timer          = 0.0f;
        int   last_sent_integer_state = -999;

        float lastValue         = 0.0f;
        float lowest_value_seen = 100.0f;
        float   tentative_low      = 100.0f;
        int64_t tentative_start_ms = 0;
    };

    FrameProcessor&  m_fp;
    RoiManager&      m_rm;
    WsClient&        m_ws;
    MatchManager&    m_match;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<int>  m_targetHz{30};

    std::atomic<double> m_actualHz{0.0};
    std::atomic<double> m_lastCycleMs{0.0};

    // CV-thread-only state (no locks needed)
    std::unordered_map<int, CachedTemplateSet> m_templateCache;
    std::unordered_map<int, RoiRuntime>        m_runtime;

    // Cross-thread comm: templates dir + per-id invalidations + full clear.
    // Snapshotted into m_cycleTemplatesDir at the start of each CV cycle.
    mutable std::mutex             m_dirtyMutex;
    std::unordered_set<int>        m_dirtyIds;
    std::unordered_set<int>        m_resetWatermarkIds;  // ResetWatermark queue
    bool                           m_dirtyAll = false;
    std::string                    m_templatesDir = "templates";

    // CV-thread-only working copy of m_templatesDir.
    std::string                    m_cycleTemplatesDir = "templates";

    // Results published to render thread
    mutable std::mutex                          m_resultsMutex;
    std::unordered_map<int, CvResult>           m_results;
};

} // namespace fightsight
