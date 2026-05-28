#pragma once

#include "App/RoiManager.h"

#include <string>
#include <vector>

namespace fightsight {

// Per-game profile management.
//
// Filesystem layout:
//   profiles/<Name>.json     - one file per profile, contains the ROI list
//   templates/<Name>/*.png   - per-profile template captures, name-isolated
//                              so SF6 "Round1Won" doesn't collide with
//                              MK1 "Round1Won"
//
// All methods that take a RoiManager& require the caller to hold
// roiManager.Mutex() across the call - same convention as RoiManager's
// direct accessors.
class ProfileManager {
public:
    bool Init(const std::string& profilesDir = "profiles",
              const std::string& templatesRoot = "templates");

    // Scan profiles directory; returns base filenames (no .json extension),
    // sorted case-insensitively.
    std::vector<std::string> ListProfiles() const;

    bool HasProfile(const std::string& name) const;

    // Replace `rm` contents with the profile's ROIs. Sets active profile
    // on success. Caller must hold rm.Mutex().
    bool LoadProfile(const std::string& name, RoiManager& rm);

    // Serialize `rm`'s current ROIs into profiles/<name>.json. Caller
    // must hold rm.Mutex().
    bool SaveProfile(const std::string& name, const RoiManager& rm) const;

    // Create a new empty profile on disk + its templates subdirectory.
    // Returns false if name is invalid or already exists.
    bool CreateProfile(const std::string& name);

    // Delete a profile's JSON. Leaves the templates folder intact (in case
    // the user wants to recover). Refuses to delete the active profile.
    bool DeleteProfile(const std::string& name);

    const std::string& ActiveProfile() const { return m_active; }
    void SetActiveProfile(const std::string& n) { m_active = n; }

    std::string ProfilesDir()       const { return m_profilesDir;  }
    std::string TemplatesRoot()     const { return m_templatesRoot; }
    std::string TemplatesDirFor(const std::string& name) const;
    std::string ActiveTemplatesDir() const { return TemplatesDirFor(m_active); }
    std::string ProfilePath(const std::string& name) const;

    // Profile names are stored as filenames - sanitize user input.
    static std::string SanitizeProfileName(const std::string& input);

private:
    std::string m_profilesDir   = "profiles";
    std::string m_templatesRoot = "templates";
    std::string m_active;
};

} // namespace fightsight
