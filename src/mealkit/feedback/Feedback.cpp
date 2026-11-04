#include "mealkit/feedback/Feedback.h"

#include "mealkit/audio/Audio.h"
#include "mealkit/core/Log.h"
#include "mealkit/data/Tables.h"
#include "mealkit/core/Random.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"

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
    f32 range = 0.0f; // 0 = anywhere
    bool positioned = false;
    Vec3 position;
    f32 phase = 0.0f; // different shape per shake
};

struct Flash {
    f64 start = 0.0;
    f32 seconds = 0.0f;
    f32 amount = 0.0f;
    Vec3 color; // linear
};

struct EventLight {
    f64 start = 0.0;
    f32 seconds = 0.0f;
    f32 range = 0.0f;
    Vec3 position;
    Vec3 color; // linear × intensity
};
constexpr size_t kMaxEventLights = 32; // the oldest goes first

struct PostKick {
    f64 start = 0.0;
    f32 seconds = 0.0f;
    f32 bloom = 0.0f, exposure = 0.0f;
};

struct ObjectFlash {
    f64 start = 0.0;
    f32 seconds = 0.0f;
    f32 amount = 0.0f;
    u32 id = 0;
};

// One live particle (view side, outside determinism)
struct Spark {
    Vec3 position, velocity;
    f32 age = 0.0f, lifetime = 1.0f;
    f32 rotation = 0.0f, spin = 0.0f;
    u32 effect = 0; // Player::Impl::effects index
};

struct LoadedEffect {
    std::string name, path;
    unsigned long long time = 0;
    bool loaded = false;
    Effect value;
};

constexpr u32 kMaxSparks = render::kMaxParticlesPerFrame;

u32 mixColor(Color a, Color b, f32 t) {
    auto ch = [&](u32 shift) {
        const f32 x = static_cast<f32>((a.value >> shift) & 0xFF), y = static_cast<f32>((b.value >> shift) & 0xFF);
        return static_cast<u32>(x + (y - x) * t + 0.5f) << shift;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

Vec3 linearColor(Color c) {
    auto ch = [](u32 v) {
        const f32 s = static_cast<f32>(v & 0xFF) / 255.0f;
        return s * s; // gamma 2.0 approximation, same as the shader
    };
    return {ch(c.value), ch(c.value >> 8), ch(c.value >> 16)};
}

// Read an event value's field (path)
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

// Name of an event kind: the first named enum field (MEALKIT_ENUM)
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
    f32 shakeScale = 1.0f, flashScale = 1.0f; // accessibility (player settings)
    bool hitStopEnabled = true;
    std::vector<Shake> shakes;
    std::vector<Flash> flashes;
    std::vector<EventLight> lights;
    std::vector<PostKick> kicks;
    std::vector<ObjectFlash> objectFlashes;
    u32 shakeCount = 0;
    std::vector<LoadedEffect> effects;
    std::vector<Spark> sparks;
    Pcg32 rng{0x6d65616c6b6974ull}; // for feedback (not mixed with simulation random numbers)
    f64 effectCheck = 0.0;

    // Effect by name (read the first time). Missing files warn and become an empty effect (pollEffects reads the file when it appears)
    u32 effectIndex(const std::string& name) {
        for (u32 i = 0; i < effects.size(); ++i)
            if (effects[i].name == name) return i;
        LoadedEffect e;
        e.name = name;
        effects.push_back(e);
        loadEffect(effects.back());
        return static_cast<u32>(effects.size() - 1);
    }
    void loadEffect(LoadedEffect& e) {
        e.path = findAsset(std::string("assets/") + kEffectFolder + "/" + e.name + ".fx");
        e.time = e.path.empty() ? 0 : fileWriteTime(e.path);
        e.value = Effect{};
        std::string error;
        e.loaded = !e.path.empty() && reflect::loadTextFile(reflect::typeOf<Effect>(), &e.value, e.path, text::kEffect, &error);
        if (!e.loaded) MEALKIT_WARN("Effect '%s': %s", e.name.c_str(), e.path.empty() ? "no assets/effects file" : error.c_str());
    }
    void pollEffects() {
        if (time < effectCheck) return;
        effectCheck = time + kTableCheckSeconds;
        for (LoadedEffect& e : effects) {
            const std::string path = findAsset(std::string("assets/") + kEffectFolder + "/" + e.name + ".fx");
            if (path != e.path || (path.empty() ? 0 : fileWriteTime(path)) != e.time) loadEffect(e);
        }
    }

    void spawn(const std::string& name, Vec3 position, f32 strength) {
        const u32 index = effectIndex(name);
        if (!effects[index].loaded) return;
        const Effect& fx = effects[index].value;
        const u32 count = static_cast<u32>(static_cast<f32>(fx.count) * std::clamp(strength, 0.0f, 1.0f) + 0.5f);
        const f32 len = length(fx.direction);
        const Vec3 dir = len > 1e-4f ? fx.direction * (1.0f / len) : Vec3{0, 1, 0};
        for (u32 i = 0; i < count && sparks.size() < kMaxSparks; ++i) {
            // Uniform point in a sphere (rejection sampling) and direction: mixes the main direction and a random direction by spread
            Vec3 ball;
            do {
                ball = Vec3{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
            } while (dot(ball, ball) > 1.0f);
            const f32 bl = length(ball);
            const Vec3 random = bl > 1e-4f ? ball * (1.0f / bl) : dir;
            Vec3 v = lerp(dir, random, std::clamp(fx.spread, 0.0f, 1.0f));
            const f32 vl = length(v);
            v = vl > 1e-4f ? v * (1.0f / vl) : random;
            Spark s;
            s.position = position + ball * fx.radius;
            s.velocity = v * (fx.speed * (1.0f - std::clamp(fx.speedRandom, 0.0f, 1.0f) * rng.next01()));
            s.lifetime = std::max(0.01f, fx.lifetime * (1.0f - std::clamp(fx.lifetimeRandom, 0.0f, 1.0f) * rng.next01()));
            s.rotation = rng.range(0.0f, kTwoPi);
            s.spin = fx.spin * kTwoPi * ((rng.next() & 1) ? 1.0f : -1.0f);
            s.effect = index;
            sparks.push_back(s);
        }
    }

    void step(f32 dt) {
        for (Spark& s : sparks) {
            const Effect& fx = effects[s.effect].value;
            s.age += dt;
            s.velocity.y -= fx.gravity * dt;
            s.velocity = s.velocity * std::max(0.0f, 1.0f - fx.drag * dt);
            s.position += s.velocity * dt;
            s.rotation += s.spin * dt;
        }
        std::erase_if(sparks, [](const Spark& s) { return s.age >= s.lifetime; });
    }

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

    void fire(const Reaction& r, f32 strength, bool positioned, Vec3 position, f32 rawStrength, const u32* target = nullptr) {
        if (r.light > 0.0f && r.lightSeconds > 0.0f && positioned) {
            if (lights.size() >= kMaxEventLights) lights.erase(lights.begin());
            lights.push_back({time, r.lightSeconds, r.lightRange, position + r.lightOffset, linearColor(r.lightColor) * (r.light * strength)});
        }
        if ((r.bloomKick != 0.0f || r.exposureKick != 0.0f) && r.kickSeconds > 0.0f)
            kicks.push_back({time, r.kickSeconds, r.bloomKick * strength, r.exposureKick * strength});
        if (r.hitFlash > 0.0f && r.hitFlashSeconds > 0.0f && target)
            objectFlashes.push_back({time, r.hitFlashSeconds, std::clamp(r.hitFlash * strength, 0.0f, 1.0f), *target});
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
        if (!r.effect.empty() && positioned) spawn(r.effect, position, strength);
        if (r.hitStop > 0.0f) hitStopUntil = std::max(hitStopUntil, time + r.hitStop);
    }
};

Player::Player(audio::Engine& audio) : m(new Impl(audio)) { m->load(); }

Player::~Player() { delete m; }

// A row whose id is the kind name or "kind.something" is feedback for that kind (multiple rows per kind: "Fall", "Fall.local")
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
            readFloat(event, r.playersFrom.empty() ? std::string("player") : r.playersFrom, player);
            const bool local = player == static_cast<f32>(localPlayer);
            if ((r.players == "local" && !local) || (r.players == "others" && local)) continue;
        }
        f32 raw = r.strengthFull;
        const bool hasStrength = readFloat(event, r.strengthFrom, raw);
        const f32 strength = hasStrength && r.strengthFull != 0.0f ? std::clamp(raw / r.strengthFull, 0.0f, 1.0f) : 1.0f;
        Vec3 position;
        const bool positioned = readVec3(event, r.positionFrom, position);
        f32 targetValue = 0.0f;
        const bool hasTarget = readFloat(event, r.targetFrom, targetValue);
        const u32 target = static_cast<u32>(targetValue);
        m->fire(r, strength, positioned, position, raw, hasTarget ? &target : nullptr);
    }
}

void Player::play(const std::string& kind, Vec3 position) {
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
    std::erase_if(m->lights, [&](const EventLight& l) { return m->time - l.start >= l.seconds; });
    std::erase_if(m->kicks, [&](const PostKick& k) { return m->time - k.start >= k.seconds; });
    std::erase_if(m->objectFlashes, [&](const ObjectFlash& f) { return m->time - f.start >= f.seconds; });
    m->pollEffects();
    if (!hitStop()) m->step(static_cast<f32>(std::min(dt, 0.1))); // particles also stop during hitstop
}

void Player::playEffect(const std::string& name, Vec3 position, f32 strength) {
    if (!name.empty()) m->spawn(name, position, strength);
}

void Player::addParticles(render::DrawList& out) const {
    if (m->sparks.empty()) return;
    std::vector<render::Particle> particles;
    particles.reserve(m->sparks.size());
    for (const Spark& s : m->sparks) {
        const Effect& fx = m->effects[s.effect].value;
        const f32 t = std::clamp(s.age / s.lifetime, 0.0f, 1.0f);
        render::Particle p;
        p.position = s.position;
        p.size = fx.sizeStart + (fx.sizeEnd - fx.sizeStart) * t;
        p.color = mixColor(fx.colorStart, fx.colorEnd, t);
        p.rotation = s.rotation;
        p.flags = fx.additive ? render::kParticleAdditive : 0u;
        p.softness = fx.softness;
        particles.push_back(p);
    }
    out.addParticles(particles.data(), particles.size());
}

u32 Player::particleCount() const { return static_cast<u32>(m->sparks.size()); }

Vec3 Player::cameraShake(Vec3 focus) const {
    Vec3 offset{};
    for (const Shake& s : m->shakes) {
        const f32 t = static_cast<f32>(m->time - s.start);
        const f32 decay = 1.0f - t / s.seconds;
        f32 amount = s.amplitude * decay * decay;
        if (s.positioned && s.range > 0.0f)
            amount *= std::clamp(1.0f - length(s.position - focus) / s.range, 0.0f, 1.0f);
        const f32 w = kTwoPi * s.frequency * t + s.phase;
        // Sum of sines with different frequencies per axis (smooth tremor)
        offset += Vec3{std::sin(w), std::sin(w * 1.31f + 1.1f), std::sin(w * 0.87f + 2.3f)} * amount;
    }
    return offset * m->shakeScale;
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
    amount *= m->flashScale;
}

void Player::addLights(std::vector<render::PointLight>& out) const {
    for (const EventLight& l : m->lights) {
        const f32 left = 1.0f - static_cast<f32>((m->time - l.start) / l.seconds);
        render::PointLight light;
        light.position = l.position;
        light.range = l.range;
        light.color = l.color * (left * left * m->flashScale);
        out.push_back(light);
    }
}

void Player::postKick(f32& bloom, f32& exposure) const {
    f32 addBloom = 0.0f, addExposure = 0.0f; // strongest wins
    for (const PostKick& k : m->kicks) {
        const f32 left = 1.0f - static_cast<f32>((m->time - k.start) / k.seconds);
        addBloom = std::max(addBloom, k.bloom * left);
        if (std::abs(k.exposure * left) > std::abs(addExposure)) addExposure = k.exposure * left;
    }
    bloom += addBloom * m->flashScale;
    exposure += addExposure * m->flashScale;
}

f32 Player::objectFlash(u32 id) const {
    f32 amount = 0.0f;
    for (const ObjectFlash& f : m->objectFlashes)
        if (f.id == id) amount = std::max(amount, f.amount * (1.0f - static_cast<f32>((m->time - f.start) / f.seconds)));
    return amount * m->flashScale;
}

bool Player::hitStop() const { return m->hitStopEnabled && m->time < m->hitStopUntil; }

void Player::setAccessibility(f32 shakeScale, f32 flashScale, bool hitStop) {
    m->shakeScale = std::clamp(shakeScale, 0.0f, 1.0f);
    m->flashScale = std::clamp(flashScale, 0.0f, 1.0f);
    m->hitStopEnabled = hitStop;
}

void Player::clear() {
    m->shakes.clear();
    m->flashes.clear();
    m->lights.clear();
    m->kicks.clear();
    m->objectFlashes.clear();
    m->hitStopUntil = 0.0;
    m->sparks.clear();
}

void Player::reload() {
    m->load();
    for (LoadedEffect& e : m->effects) m->loadEffect(e);
}

const std::vector<std::string>& Player::rowIds() const { return m->table.ids; }

} // namespace mealkit::feedback
