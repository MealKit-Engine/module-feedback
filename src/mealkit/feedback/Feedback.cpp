#include "mealkit/feedback/Feedback.h"

#include "mealkit/audio/Audio.h"
#include "mealkit/core/Log.h"
#include "mealkit/data/Tables.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mealkit::feedback {
namespace {

constexpr f64 kTableCheckSeconds = 0.25;
constexpr f32 kTwoPi = 6.28318531f;

struct Shake {
    f64 start = 0.0;
    f32 seconds = 0.0f;
    f32 amplitude = 0.0f;
    f32 frequency = 0.0f;
    f32 range = 0.0f; // 0 = 어디서나
    bool positioned = false;
    Vec3 position;
    f32 phase = 0.0f; // 흔들림마다 다른 모양
};

struct Flash {
    f64 start = 0.0;
    f32 seconds = 0.0f;
    f32 amount = 0.0f;
    Vec3 color; // 선형
};

Vec3 linearColor(Color c) {
    auto ch = [](u32 v) {
        const f32 s = static_cast<f32>(v & 0xFF) / 255.0f;
        return s * s; // 셰이더와 같은 감마 2.0 근사
    };
    return {ch(c.value), ch(c.value >> 8), ch(c.value >> 16)};
}

// 이벤트 값의 필드 (경로) 읽기
bool readFloat(reflect::View event, const std::string& path, f32& out) {
    const reflect::ValueInfo* v = nullptr;
    void* data = nullptr;
    if (path.empty() || !reflect::resolvePath(*event.type, const_cast<void*>(event.data), path, v, data)) return false;
    switch (v->kind) {
    case reflect::Kind::Float: out = *static_cast<const f32*>(data); return true;
    case reflect::Kind::Int:
    case reflect::Kind::UInt: {
        i64 x = 0;
        std::memcpy(&x, data, v->size);
        if (v->kind == reflect::Kind::Int && v->size < 8 && (x >> (v->size * 8 - 1)) & 1) x -= i64(1) << (v->size * 8);
        out = static_cast<f32>(x);
        return true;
    }
    case reflect::Kind::Bool: out = *static_cast<const bool*>(data) ? 1.0f : 0.0f; return true;
    default: return false;
    }
}

bool readVec3(reflect::View event, const std::string& path, Vec3& out) {
    const reflect::ValueInfo* v = nullptr;
    void* data = nullptr;
    if (path.empty() || !reflect::resolvePath(*event.type, const_cast<void*>(event.data), path, v, data)) return false;
    if (v->kind != reflect::Kind::Vec3) return false;
    out = *static_cast<const Vec3*>(data);
    return true;
}

// 이벤트 종류의 이름: 첫 번째 이름 있는 enum 필드 (MEALKIT_ENUM)
std::string kindName(reflect::View event) {
    for (const reflect::FieldInfo& f : event.type->fields)
        if (!f.value->enumNames.empty())
            return reflect::formatScalar(*f.value, static_cast<const u8*>(event.data) + f.offset);
    return {};
}

} // namespace

struct Player::Impl {
    audio::Engine& audio;
    sim::Table<Reaction> table;
    std::string tablePath;
    unsigned long long tableTime = 0;
    f64 tableCheck = 0.0;
    f64 time = 0.0;
    f64 hitStopUntil = 0.0;
    std::vector<Shake> shakes;
    std::vector<Flash> flashes;
    u32 shakeCount = 0;

    explicit Impl(audio::Engine& a) : audio(a) {}

    void load() {
        tablePath = findAsset(std::string("assets/") + kFeedbackTable);
        tableTime = tablePath.empty() ? 0 : fileWriteTime(tablePath);
        table = {};
        if (!tablePath.empty()) data::loadTable(table, kFeedbackTable);
    }

    void poll() {
        if (time < tableCheck) return;
        tableCheck = time + kTableCheckSeconds;
        const std::string path = findAsset(std::string("assets/") + kFeedbackTable);
        const unsigned long long t = path.empty() ? 0 : fileWriteTime(path);
        if (path != tablePath || t != tableTime) {
            load();
            MEALKIT_INFO("Feedback: %u reaction(s) from %s", table.size(), path.empty() ? "(no feedback.table)" : path.c_str());
        }
    }

    void fire(const Reaction& r, f32 strength, bool positioned, Vec3 position, f32 rawStrength) {
        if (!r.sound.empty()) {
            if (positioned) {
                if (r.soundParam.empty()) audio.play(r.sound, position);
                else audio.play(r.sound, position, {{r.soundParam.c_str(), rawStrength}});
            } else {
                if (r.soundParam.empty()) audio.play2D(r.sound);
                else audio.play2D(r.sound, {{r.soundParam.c_str(), rawStrength}});
            }
        }
        if (r.shake > 0.0f && r.shakeSeconds > 0.0f)
            shakes.push_back({time, r.shakeSeconds, r.shake * strength, r.shakeFrequency, r.shakeRange, positioned,
                              position, static_cast<f32>(shakeCount++) * 1.7f});
        if (r.flash > 0.0f && r.flashSeconds > 0.0f)
            flashes.push_back({time, r.flashSeconds, std::clamp(r.flash * strength, 0.0f, 1.0f), linearColor(r.flashColor)});
        if (r.hitStop > 0.0f) hitStopUntil = std::max(hitStopUntil, time + r.hitStop);
    }
};

Player::Player(audio::Engine& audio) : m(new Impl(audio)) { m->load(); }

Player::~Player() { delete m; }

// 행 id가 종류 이름이거나 "종류.무엇"이면 그 종류의 연출이다 (한 종류에 여러 줄: "Fall", "Fall.local")
void Player::trigger(reflect::View event, u8 localPlayer) {
    if (!event.type || !event.data) return;
    const std::string kind = kindName(event);
    if (kind.empty()) return;
    for (u32 row = 0; row < m->table.size(); ++row) {
        const std::string& id = m->table.id(row);
        if (id != kind && !(id.size() > kind.size() && id.compare(0, kind.size(), kind) == 0 && id[kind.size()] == '.'))
            continue;
        const Reaction& r = m->table[row];
        if (r.players != "any") {
            f32 player = -1.0f;
            readFloat(event, "player", player);
            const bool local = player == static_cast<f32>(localPlayer);
            if ((r.players == "local" && !local) || (r.players == "others" && local)) continue;
        }
        f32 raw = r.strengthFull;
        const bool hasStrength = readFloat(event, r.strengthFrom, raw);
        const f32 strength = hasStrength && r.strengthFull != 0.0f ? std::clamp(raw / r.strengthFull, 0.0f, 1.0f) : 1.0f;
        Vec3 position;
        const bool positioned = readVec3(event, r.positionFrom, position);
        m->fire(r, strength, positioned, position, raw);
    }
}

void Player::preview(const std::string& kind, Vec3 position) {
    const u32 row = m->table.find(kind);
    if (row == kNoRow) return;
    const Reaction& r = m->table[row];
    m->fire(r, 1.0f, !r.positionFrom.empty(), position, r.strengthFull);
}

void Player::update(f64 dt) {
    m->time += dt;
    m->poll();
    std::erase_if(m->shakes, [&](const Shake& s) { return m->time - s.start >= s.seconds; });
    std::erase_if(m->flashes, [&](const Flash& f) { return m->time - f.start >= f.seconds; });
}

Vec3 Player::cameraShake(Vec3 focus) const {
    Vec3 offset{};
    for (const Shake& s : m->shakes) {
        const f32 t = static_cast<f32>(m->time - s.start);
        const f32 decay = 1.0f - t / s.seconds;
        f32 amount = s.amplitude * decay * decay;
        if (s.positioned && s.range > 0.0f)
            amount *= std::clamp(1.0f - length(s.position - focus) / s.range, 0.0f, 1.0f);
        const f32 w = kTwoPi * s.frequency * t + s.phase;
        // 축마다 다른 주파수의 사인 합 (부드러운 떨림)
        offset += Vec3{std::sin(w), std::sin(w * 1.31f + 1.1f), std::sin(w * 0.87f + 2.3f)} * amount;
    }
    return offset;
}

void Player::screenTint(Vec3& color, f32& amount) const {
    amount = 0.0f;
    for (const Flash& f : m->flashes) {
        const f32 a = f.amount * (1.0f - static_cast<f32>((m->time - f.start) / f.seconds));
        if (a > amount) {
            amount = a;
            color = f.color;
        }
    }
}

bool Player::hitStop() const { return m->time < m->hitStopUntil; }

void Player::clear() {
    m->shakes.clear();
    m->flashes.clear();
    m->hitStopUntil = 0.0;
}

void Player::reload() { m->load(); }

const std::vector<std::string>& Player::rowIds() const { return m->table.ids; }

} // namespace mealkit::feedback
