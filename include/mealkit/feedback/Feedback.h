#pragma once

#include "mealkit/core/Math.h"
#include "mealkit/core/Reflect.h"

#include <string>
#include <vector>

namespace mealkit::audio {
class Engine;
}

// 이벤트 → 연출 표 (docs/CODING_MODEL.md "시뮬레이션 이벤트에 소리·효과·흔들림 붙이기"): 타격감을 데이터로.
// assets/feedback.table의 한 행 = 이벤트 종류(게임 이벤트 타입의 MEALKIT_ENUM 값 이름) → 소리 + 화면 흔들림 + 번쩍임 + 화면 정지.
// 게임은 틱 이벤트마다 ctx.feedback.trigger(event, localPlayer)만 부르고, 무엇을 할지는 표가 정한다(에디터 [연출] 패널).
// 전부 화면 쪽이다: 화면 정지(히트 스톱)도 화면만 멈추고 시뮬레이션은 계속 돈다(락스텝). 표는 콘텐츠 해시 밖이라 플레이 중에 고치면 바로 보인다.
namespace mealkit::feedback {

// 연출 한 행. 필드 설명은 에디터 글자라 영어 (한국어는 engine/res/strings/ko.txt)
MEALKIT_STRUCT(Reaction) {
    MEALKIT_FIELD(std::string, players, "any", "Whose events (any, local, others)");
    MEALKIT_FIELD(std::string, strengthFrom, {}, "Strength from event field (empty is full)");
    MEALKIT_FIELD(f32, strengthFull, 1.0f, "Field value at full strength");
    MEALKIT_FIELD(std::string, positionFrom, "position", "Position from event field (empty is everywhere)");
    MEALKIT_FIELD(std::string, sound, {}, "Sound event");
    MEALKIT_FIELD(std::string, soundParam, {}, "Sound parameter set to the strength field");
    MEALKIT_FIELD(f32, shake, 0.0f, "Camera shake at full strength (m)");
    MEALKIT_FIELD(f32, shakeSeconds, 0.25f, "Shake time (s)");
    MEALKIT_FIELD(f32, shakeFrequency, 18.0f, "Shake frequency (Hz)");
    MEALKIT_FIELD(f32, shakeRange, 30.0f, "Shake fades out at this distance (m, 0 is everywhere)");
    MEALKIT_FIELD(f32, flash, 0.0f, "Screen flash at full strength (0 to 1)");
    MEALKIT_FIELD(f32, flashSeconds, 0.12f, "Flash time (s)");
    MEALKIT_FIELD(Color, flashColor, (Color::rgb(255, 255, 255)), "Flash color");
    MEALKIT_FIELD(f32, hitStop, 0.0f, "Hit stop: view freezes (s)");
};

inline constexpr const char* kFeedbackTable = "feedback.table"; // assets 기준

class Player {
public:
    explicit Player(audio::Engine& audio);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // 틱 이벤트 하나. 이벤트 타입은 MEALKIT_STRUCT이고 종류는 MEALKIT_ENUM 필드(첫 번째 이름 있는 enum)다.
    // localPlayer: 이 창의 플레이어 (players가 local/others인 행, 이벤트의 "player" 필드와 비교)
    void trigger(reflect::View event, u8 localPlayer);
    template <class E>
    void trigger(const E& event, u8 localPlayer) {
        trigger({&reflect::typeOf<E>(), &event}, localPlayer);
    }
    // 에디터 [연출] 패널의 미리보기: 행 이름으로 (세기 1, 위치 = 카메라 초점)
    void preview(const std::string& kind, Vec3 position);

    // 매 프레임 (엔진이 부른다): 시간, 표 다시 읽기(파일이 바뀌면)
    void update(f64 dt);
    // 카메라에 더할 흔들림 (focus: 카메라가 보는 점, 거리 감쇠용). 게임 카메라가 눈과 초점에 같이 더한다
    Vec3 cameraShake(Vec3 focus) const;
    // 화면 번쩍임: 엔진이 FrameParams::tintColor/tintAmount에 넣는다
    void screenTint(Vec3& color, f32& amount) const;
    // 화면 정지 중 (게임 화면 쪽 시계·보간·효과를 멈춘다. 시뮬레이션은 계속)
    bool hitStop() const;
    void clear(); // 진행 중인 흔들림·번쩍임·정지를 지운다 (플레이를 멈출 때)
    // 접근성 (플레이어 설정): 흔들림·번쩍임 배율 0..1, 화면 정지 켜기. 표는 그대로이고 결과에만 곱한다
    void setAccessibility(f32 shakeScale, f32 flashScale, bool hitStop);

    void reload(); // 표를 지금 다시 읽는다 (에디터가 저장한 뒤)
    const std::vector<std::string>& rowIds() const;

private:
    struct Impl;
    Impl* m = nullptr;
};

} // namespace mealkit::feedback
