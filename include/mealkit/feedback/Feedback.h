#pragma once

#include "mealkit/core/Math.h"
#include "mealkit/core/Reflect.h"

#include <string>
#include <vector>

namespace mealkit::audio {
class Engine;
}
namespace mealkit::render {
class DrawList;
struct PointLight;
}

// Event → feedback table (docs/CODING_MODEL.md "Attaching sound, effects, and shake to simulation events"): game feel as data.
// One row of assets/feedback.table = event kind (MEALKIT_ENUM value name of the game event type) → sound + screen shake + flash + hitstop.
// The game just calls ctx.feedback.trigger(event, localPlayer) per tick event, and the table decides what happens (editor [Feedback] panel).
// It's all view side: hitstop only freezes the screen while the simulation keeps running (lockstep). The table is outside the content hash, so edits during play show immediately.
namespace mealkit::feedback {

// One feedback row. Field descriptions are editor text, so English (Korean in engine/res/strings/ko.txt)
MEALKIT_STRUCT(Reaction) {
    MEALKIT_FIELD(std::string, players, "any", "Whose events (any, local, others)");
    MEALKIT_FIELD(std::string, strengthFrom, {}, "Strength from event field (empty is full)");
    MEALKIT_FIELD(f32, strengthFull, 1.0f, "Field value at full strength");
    MEALKIT_FIELD(std::string, positionFrom, "position", "Position from event field (empty is everywhere)");
    MEALKIT_FIELD(std::string, sound, {}, "Sound event|pick=sound");
    MEALKIT_FIELD(std::string, soundParam, {}, "Sound parameter set to the strength field|showIf=sound");
    MEALKIT_FIELD(f32, shake, 0.0f, "Camera shake at full strength (m)");
    MEALKIT_FIELD(f32, shakeSeconds, 0.25f, "Shake time (s)|showIf=shake>0");
    MEALKIT_FIELD(f32, shakeFrequency, 18.0f, "Shake frequency (Hz)|showIf=shake>0");
    MEALKIT_FIELD(f32, shakeRange, 30.0f, "Shake fades out at this distance (m, 0 is everywhere)|showIf=shake>0");
    MEALKIT_FIELD(f32, flash, 0.0f, "Screen flash at full strength (0 to 1)");
    MEALKIT_FIELD(f32, flashSeconds, 0.12f, "Flash time (s)|showIf=flash>0");
    MEALKIT_FIELD(Color, flashColor, (Color::rgb(255, 255, 255)), "Flash color|showIf=flash>0");
    MEALKIT_FIELD(f32, hitStop, 0.0f, "Hit stop: view freezes (s)");
    MEALKIT_FIELD(std::string, effect, {}, "Particle effect (assets/effects/<name>.fx)|pick=effects:.fx");
    MEALKIT_FIELD(std::string, playersFrom, {}, "Event field compared by players (empty is player)|showIf=players");
    MEALKIT_FIELD(f32, light, 0.0f, "Event light intensity at full strength");
    MEALKIT_FIELD(Color, lightColor, (Color::rgb(255, 255, 255)), "Light color|showIf=light>0");
    MEALKIT_FIELD(f32, lightRange, 4.0f, "Light range (m)|showIf=light>0");
    MEALKIT_FIELD(f32, lightSeconds, 0.15f, "Light fade time (s)|showIf=light>0");
    MEALKIT_FIELD(Vec3, lightOffset, (Vec3{0.0f, 0.5f, 0.0f}), "Light offset from the event position|showIf=light>0");
    MEALKIT_FIELD(f32, bloomKick, 0.0f, "Bloom added at full strength");
    MEALKIT_FIELD(f32, exposureKick, 0.0f, "Exposure added at full strength (EV)");
    MEALKIT_FIELD(f32, kickSeconds, 0.2f, "Bloom and exposure kick time (s)");
    MEALKIT_FIELD(f32, hitFlash, 0.0f, "Object flash at full strength (0 to 1)");
    MEALKIT_FIELD(f32, hitFlashSeconds, 0.1f, "Object flash time (s)|showIf=hitFlash>0");
    MEALKIT_FIELD(std::string, targetFrom, "player", "Event field with the id that flashes|showIf=hitFlash>0");
};

// One particle effect = assets/effects/<name>.fx (docs/DECISIONS.md 2026-10-27 "Particles use our own runtime"). A burst of particles emitted at once:
// view-side CPU update + instanced quads (render::Particle). Values are edited in the effects section of the editor [Feedback] panel and checked with preview
MEALKIT_STRUCT(Effect) {
    MEALKIT_FIELD(u32, count, 24, "Particles at full strength");
    MEALKIT_FIELD(f32, lifetime, 0.6f, "Lifetime (s)");
    MEALKIT_FIELD(f32, lifetimeRandom, 0.3f, "Lifetime random part (0 to 1)");
    MEALKIT_FIELD(f32, radius, 0.1f, "Start inside this sphere (m)");
    MEALKIT_FIELD(Vec3, direction, (Vec3{0.0f, 1.0f, 0.0f}), "Main direction");
    MEALKIT_FIELD(f32, spread, 0.6f, "Spread (0 straight, 1 every way)");
    MEALKIT_FIELD(f32, speed, 4.0f, "Speed (m/s)");
    MEALKIT_FIELD(f32, speedRandom, 0.5f, "Speed random part (0 to 1)");
    MEALKIT_FIELD(f32, gravity, 6.0f, "Gravity (m/s2, down)");
    MEALKIT_FIELD(f32, drag, 2.0f, "Drag (1/s)");
    MEALKIT_FIELD(f32, sizeStart, 0.25f, "Size at start (m)");
    MEALKIT_FIELD(f32, sizeEnd, 0.05f, "Size at end (m)");
    MEALKIT_FIELD(Color, colorStart, (Color::rgb(255, 220, 140)), "Color at start");
    MEALKIT_FIELD(Color, colorEnd, (Color::rgb(255, 80, 20, 0)), "Color at end");
    MEALKIT_FIELD(f32, softness, 0.6f, "Edge softness (0 to 1)");
    MEALKIT_FIELD(f32, spin, 0.0f, "Spin (turns per second, random sign)");
    MEALKIT_FIELD(bool, additive, true, "Additive (glow)");
};
inline constexpr const char* kEffectFolder = "effects"; // relative to assets, files are <name>.fx

inline constexpr const char* kFeedbackTable = "feedback.table"; // relative to assets

class Player {
public:
    explicit Player(audio::Engine& audio);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // One tick event. The event type is a MEALKIT_STRUCT and its kind is a MEALKIT_ENUM field (the first named enum).
    // localPlayer: this window's player (for rows whose players is local/others, compared with the event's "player" field)
    void trigger(reflect::View event, u8 localPlayer);
    template <class E>
    void trigger(const E& event, u8 localPlayer) {
        trigger({&reflect::typeOf<E>(), &event}, localPlayer);
    }
    // By row name (strength 1): animation events (asset::updateAnim) and the editor [Feedback] panel preview (position = camera focus)
    void play(const std::string& kind, Vec3 position);
    void preview(const std::string& kind, Vec3 position) { play(kind, position); }
    // Spawns one effect right away (editor effect preview, game view code). The file is reread when it changes
    void playEffect(const std::string& name, Vec3 position, f32 strength = 1.0f);
    // Puts live particles into the draw list (the engine calls this after buildFrame, before submit)
    void addParticles(render::DrawList& out) const;
    u32 particleCount() const;

    // Every frame (called by the engine): time, reload the table (if the file changed)
    void update(f64 dt);
    // Shake to add to the camera (focus: point the camera looks at, for distance attenuation). The game camera adds it to both eye and focus
    Vec3 cameraShake(Vec3 focus) const;
    // Screen flash: the engine puts it into FrameParams::tintColor/tintAmount
    void screenTint(Vec3& color, f32& amount) const;
    // Event lights, appended to out (the engine adds them to FrameParams::lights)
    void addLights(std::vector<render::PointLight>& out) const;
    // Bloom and exposure kicks to add to the frame's post values (the engine does this)
    void postKick(f32& bloom, f32& exposure) const;
    // Flash amount 0..1 of an id from the row's targetFrom field. View code passes it to render::instanceFlash when drawing that object
    f32 objectFlash(u32 id) const;
    // During hitstop (freezes game view clocks/interpolation/effects. The simulation continues)
    bool hitStop() const;
    void clear(); // clears ongoing shake/flash/hitstop (when stopping play)
    // Accessibility (player settings): shake/flash scale 0..1, hitstop on/off. The table stays the same; only results are multiplied
    void setAccessibility(f32 shakeScale, f32 flashScale, bool hitStop);

    void reload(); // reread the table now (after the editor saves)
    const std::vector<std::string>& rowIds() const;

private:
    struct Impl;
    Impl* m = nullptr;
};

} // namespace mealkit::feedback
