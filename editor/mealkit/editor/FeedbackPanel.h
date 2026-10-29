#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"
#include "mealkit/feedback/Feedback.h"

#include <string>

// Editor [Feedback] panel: choose feedback (sound/shake/flash/hitstop) per event kind (assets/feedback.table).
// Left shows kind names of the game event type (GameClass::EventType), right the feedback rows of the selected kind (multiple rows per kind like "Fall.local").
// The [Effects] section below picks particle effects (assets/effects/<name>.fx) and edits values (autosaved together, [Preview Effect]).
// Edits are saved after 0.3 s and the game's feedback::Player rereads the file so they show immediately during play. [Preview] fires one right now.
namespace mealkit::editor {

class FeedbackPanel {
public:
    void setProjectDirectory(const std::string& directory);
    // Game event type (GameModule::eventType, nullptr before unloading the DLL)
    void setEventType(const reflect::TypeInfo* type) { m_eventType = type; }
    void draw(feedback::Player* player, Vec3 previewAt, f64 dt, bool* open);

private:
    void load();
    void save();
    void drawEffects(feedback::Player* player, Vec3 previewAt);
    void loadEffect(const std::string& name);
    void saveEffect();

    std::string m_path;
    const reflect::TypeInfo* m_eventType = nullptr;
    data::TableData m_table;
    std::string m_selectedKind;
    bool m_dirty = false;
    f64 m_saveTimer = 0.0;
    std::string m_status;
    char m_suffix[32] = "local";
    // Particle effect editing (assets/effects)
    std::string m_effectsDir;
    std::string m_effectName; // selected effect (empty = none)
    feedback::Effect m_effect;
    bool m_effectDirty = false;
    char m_newEffect[48] = "spark";
};

} // namespace mealkit::editor
