#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"
#include "mealkit/feedback/Feedback.h"

#include <string>

// 에디터 [연출] 패널: 이벤트 종류마다 연출(소리·흔들림·번쩍임·화면 정지)을 고른다 (assets/feedback.table).
// 왼쪽은 게임 이벤트 타입(GameClass::EventType)의 종류 이름, 오른쪽은 고른 종류의 연출 행(같은 종류에 "Fall.local"처럼 여러 행).
// 아래 [효과] 칸은 파티클 효과(assets/effects/<이름>.fx)를 고르고 값을 고친다 (같이 자동 저장, [효과 미리보기]).
// 고치면 0.3초 뒤 저장하고, 게임의 feedback::Player가 파일을 다시 읽어 플레이 중에 바로 보인다. [미리보기]는 지금 바로 한 번 낸다.
namespace mealkit::editor {

class FeedbackPanel {
public:
    void setProjectDirectory(const std::string& directory);
    // 게임 이벤트 타입 (GameModule::eventType, DLL을 내리기 전에 nullptr)
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
    // 파티클 효과 편집 (assets/effects)
    std::string m_effectsDir;
    std::string m_effectName; // 고른 효과 (비면 없음)
    feedback::Effect m_effect;
    bool m_effectDirty = false;
    char m_newEffect[48] = "spark";
};

} // namespace mealkit::editor
