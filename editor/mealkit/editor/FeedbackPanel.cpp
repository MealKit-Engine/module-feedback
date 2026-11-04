#include "mealkit/editor/FeedbackPanel.h"

#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cstring>

#include <imgui.h>

namespace mealkit::editor {
namespace {

// "Fall", "Fall.local" → "Fall"
std::string kindOf(const std::string& rowId) {
    const size_t dot = rowId.find('.');
    return dot == std::string::npos ? rowId : rowId.substr(0, dot);
}

const char* kindName(const reflect::ValueInfo& v) {
    switch (v.kind) {
    case reflect::Kind::Float: return "f32";
    case reflect::Kind::Vec3: return "Vec3";
    case reflect::Kind::Int:
    case reflect::Kind::UInt: return v.enumNames.empty() ? "int" : "enum";
    case reflect::Kind::Bool: return "bool";
    default: return "";
    }
}

} // namespace

void FeedbackPanel::setProjectDirectory(const std::string& directory) {
    m_path = directory + "\\assets\\" + feedback::kFeedbackTable;
    m_effectsDir = directory + "\\assets\\" + feedback::kEffectFolder;
    m_effectName.clear();
    load();
}

void FeedbackPanel::load() {
    std::string warnings;
    m_table = {};
    m_table.rowType = &reflect::typeOf<feedback::Reaction>();
    if (fileExists(m_path)) data::loadTableData(*m_table.rowType, m_path, m_table, &warnings);
    m_dirty = false;
    m_status = warnings;
    data::TableText now;
    data::tableToText(m_table, now);
    m_undo.reset(std::move(now));
}

void FeedbackPanel::step(bool back) {
    data::TableText text;
    if (!m_table.rowType || !m_undo.step(back, text)) return;
    const reflect::TypeInfo* rowType = m_table.rowType;
    m_table = {};
    std::string warnings;
    data::tableFromText(*rowType, text, m_table, &warnings);
    m_dirty = true;
}

void FeedbackPanel::save() {
    createDirectories(parentDirectory(m_path));
    m_status = data::saveTableData(m_table, m_path) ? trf("Saved %s", "assets\\feedback.table")
                                                     : trf("Could not save %s", m_path.c_str());
    m_dirty = false;
}

void FeedbackPanel::loadEffect(const std::string& name) {
    m_effectName = name;
    m_effect = {};
    m_effectDirty = false;
    std::string error;
    const std::string path = m_effectsDir + "\\" + name + ".fx";
    if (fileExists(path) && !reflect::loadTextFile(reflect::typeOf<feedback::Effect>(), &m_effect, path, text::kEffect, &error))
        m_status = error;
    reflect::TextFields now;
    reflect::toText(reflect::typeOf<feedback::Effect>(), &m_effect, now);
    m_effectUndo.reset(std::move(now));
}

void FeedbackPanel::stepEffect(bool back) {
    reflect::TextFields text;
    if (m_effectName.empty() || !m_effectUndo.step(back, text)) return;
    m_effect = {};
    reflect::fromText(reflect::typeOf<feedback::Effect>(), &m_effect, text);
    m_effectDirty = true;
}

void FeedbackPanel::saveEffect() {
    m_effectDirty = false;
    if (m_effectName.empty()) return;
    createDirectories(m_effectsDir);
    const std::string path = m_effectsDir + "\\" + m_effectName + ".fx";
    if (!reflect::saveTextFile(reflect::typeOf<feedback::Effect>(), &m_effect, path, text::kEffect,
                               "MealKit particle effect. Edit it in the editor's [Feedback] panel."))
        m_status = trf("Could not save %s", path.c_str());
}

// [Effects] section: pick/create effect files, edit values, preview (the effect column of feedback rows uses this name)
void FeedbackPanel::drawEffects(feedback::Player* player, Vec3 previewAt) {
    std::vector<std::string> names;
    for (const std::string& file : listFiles(m_effectsDir, ".fx")) names.push_back(file.substr(0, file.size() - 3));
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##effect", m_effectName.empty() ? tr("(choose an effect)") : m_effectName.c_str())) {
        for (const std::string& n : names)
            if (ImGui::Selectable(n.c_str(), n == m_effectName)) {
                if (m_effectDirty) saveEffect();
                loadEffect(n);
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!player || m_effectName.empty());
    if (ImGui::Button(tr("Preview Effect")) && player) {
        if (m_effectDirty) saveEffect();
        player->reload();
        player->playEffect(m_effectName, previewAt);
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("##newEffect", m_newEffect, sizeof(m_newEffect));
    ImGui::SameLine();
    if (ImGui::Button(tr("New Effect"))) {
        const std::string name = m_newEffect;
        if (!data::validRowId(name) || std::find(names.begin(), names.end(), name) != names.end()) {
            m_status = trf("'%s' cannot be an effect name (letters, digits, _ - . and unique)", name.c_str());
        } else {
            if (m_effectDirty) saveEffect();
            m_effectName = name;
            m_effect = {};
            saveEffect();
            reflect::TextFields now;
            reflect::toText(reflect::typeOf<feedback::Effect>(), &m_effect, now);
            m_effectUndo.reset(std::move(now));
        }
    }
    if (!m_effectName.empty()) {
        ImGui::TextDisabled("%s", trf("Use it in a reaction's effect column: %s", m_effectName.c_str()).c_str());
        ImGui::PushID("effect");
        ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.5f);
        if (drawFields(reflect::typeOf<feedback::Effect>(), &m_effect)) {
            m_effectDirty = true;
            m_effectUndo.mark();
        }
        ImGui::PopItemWidth();
        m_effectUndo.settle(ImGui::IsAnyItemActive(), [this] {
            reflect::TextFields now;
            reflect::toText(reflect::typeOf<feedback::Effect>(), &m_effect, now);
            return now;
        });
        ImGui::PopID();
    }
}

void FeedbackPanel::update(f64 dt) {
    if (m_dirty || m_effectDirty) { // while editing, save at most once every 0.3 s (the game rereads it so it shows immediately)
        m_saveTimer += dt;
        if (m_saveTimer >= 0.3) {
            if (m_dirty) save();
            if (m_effectDirty) saveEffect();
            m_saveTimer = 0.0;
        }
    }
}

void FeedbackPanel::drawContent(feedback::Player* player, Vec3 previewAt) {
    if (m_path.empty()) return;

    // ---- Left: event kinds (the first named enum of the game event type + names only in the table)
    std::vector<std::string> kinds;
    const reflect::FieldInfo* kindField = nullptr;
    if (m_eventType)
        for (const reflect::FieldInfo& f : m_eventType->fields)
            if (!f.value->enumNames.empty()) {
                kindField = &f;
                kinds = f.value->enumNames;
                break;
            }
    for (const std::string& id : m_table.ids)
        if (std::find(kinds.begin(), kinds.end(), kindOf(id)) == kinds.end()) kinds.push_back(kindOf(id));
    if (m_selectedKind.empty() && !kinds.empty()) m_selectedKind = kinds[0];

    ImGui::BeginChild("##kinds", ImVec2(160.0f, 0.0f), ImGuiChildFlags_Borders);
    if (!m_eventType) ImGui::TextDisabled("%s", tr("Event type appears when the game code is loaded (EventType)."));
    for (const std::string& k : kinds) {
        u32 rows = 0;
        for (const std::string& id : m_table.ids) rows += kindOf(id) == k ? 1u : 0u;
        const std::string label = k + (rows ? "  (" + std::to_string(rows) + ")" : std::string());
        if (ImGui::Selectable(label.c_str(), k == m_selectedKind)) m_selectedKind = k;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- Right: feedback rows of the selected kind
    ImGui::BeginChild("##rows");
    ImGui::TextUnformatted(m_selectedKind.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!player);
    if (ImGui::Button(tr("Preview")) && player) {
        if (m_dirty) save();
        player->reload();
        for (const std::string& id : m_table.ids)
            if (kindOf(id) == m_selectedKind) player->preview(id, previewAt);
    }
    ImGui::EndDisabled();
    if (m_eventType) { // event fields selectable in feedback values
        std::string fields;
        for (const reflect::FieldInfo& f : m_eventType->fields) {
            if (&f == kindField || !*kindName(*f.value)) continue;
            fields += (fields.empty() ? "" : ", ") + std::string(f.name) + " (" + kindName(*f.value) + ")";
        }
        ImGui::TextDisabled("%s", trf("Event fields: %s", fields.c_str()).c_str());
    }
    ImGui::TextDisabled("%s", tr("Saved automatically; a playing game picks the change up at once."));
    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());
    ImGui::Separator();

    int remove = -1;
    for (size_t i = 0; i < m_table.ids.size(); ++i) {
        if (kindOf(m_table.ids[i]) != m_selectedKind) continue;
        ImGui::PushID(static_cast<int>(i));
        const bool openRow = sectionHeader(m_table.ids[i].c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::SmallButton(tr("Delete"))) remove = static_cast<int>(i);
        if (openRow) {
            ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.5f);
            if (drawFields(*m_table.rowType, m_table.rows[i].data())) {
                m_dirty = true;
                m_undo.mark();
            }
            ImGui::PopItemWidth();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        m_table.ids.erase(m_table.ids.begin() + remove);
        m_table.rows.erase(m_table.rows.begin() + remove);
        m_dirty = true;
        m_undo.mark();
    }
    ImGui::Separator();
    if (ImGui::Button(tr("Add Reaction")) && !m_selectedKind.empty()) {
        // The first row is the kind name as is, then "kind.suffix" (e.g. Fall.local)
        std::string id = m_selectedKind;
        if (std::find(m_table.ids.begin(), m_table.ids.end(), id) != m_table.ids.end()) id += std::string(".") + m_suffix;
        if (!data::validRowId(id) || std::find(m_table.ids.begin(), m_table.ids.end(), id) != m_table.ids.end()) {
            m_status = trf("'%s' cannot be a row id (letters, digits, _ - . and unique in the table)", id.c_str());
        } else {
            m_table.ids.push_back(id);
            m_table.rows.emplace_back(*m_table.rowType);
            m_dirty = true;
            m_undo.mark();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputText(tr("suffix for more rows"), m_suffix, sizeof(m_suffix));
    ImGui::EndChild();
    m_undo.settle(ImGui::IsAnyItemActive(), [this] {
        data::TableText now;
        data::tableToText(m_table, now);
        return now;
    });
}

} // namespace mealkit::editor
