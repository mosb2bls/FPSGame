#pragma once
#include <string>
#include <algorithm>
#include "Window.h"
#include "Animation.h"

// FPS viewmodel state machine with ADS (zoom) + reload interrupt rules.
// - Hold RMB (mouseButtons[2]) to zoom (ADS)
// - Zoom uses zoom/zoomWalk/zoomFire animations if available
// - Gun offset changes when zooming
// - If zooming and R pressed: force unzoom -> reload -> if RMB still held, zoom again
class modelState
{
public:
    // --- Key / mouse mapping (per your Window.cpp) ---
    int fireMouseButton = 0;   // LMB  (mouseButtons[0])
    int zoomMouseButton = 2;   // RMB  (mouseButtons[2])
    int reloadKey = 'R';

    // --- Clips (set these to EXACT names in your .gem) ---
    std::string idleClip = "04 idle";
    std::string walkClip = "07 walk";
    std::string fireClip = "08 fire";
    std::string reloadClip = "17 reload";

    std::string zoomIdleClip = "zoom";
    std::string zoomWalkClip = "zoom walk";
    std::string zoomFireClip = "zoom fire";

    // --- Fire tuning ---
    bool  allowHoldFire = true;
    float shotsPerSecond = 12.0f;  // continuous fire rate while holding LMB
    float fireAnimRate = 3.0f;   // speed multiplier for firing anim

    // --- Locomotion tuning ---
    float idleAnimRate = 1.0f;
    float walkAnimRate = 1.0f;
    float zoomIdleRate = 1.0f;
    float zoomWalkRate = 1.0f;

    // --- Reload tuning ---
    float reloadAnimRate = 1.0f;

    const float PI = 3.141592654f;
    // --- Viewmodel offsets (these are your gunX/Y/Z in view space) ---
    // Normal (hip fire)
    float gunX = 0.08f;
    float gunY = 0.00f;
    float gunZ = 0.00f;
    float modelRotY = +PI * 1.01f;

    // Zoom (ADS) - tweak these
    float zoomGunX = -0.04f;
    float zoomGunY = 0.02f;
    float zoomGunZ = 0.0f;
    float zoommodelRotY = +PI;

public:
    // Call each frame. Returns true if a shot should be spawned this frame.
    bool update(Window& w, AnimationInstance& inst, float dt)
    {
        if (!inst.animation) return false;
        dt = std::min(dt, 0.05f);

        // ---- zoom intent ----
        bool wantZoom = (w.mouseButtons[zoomMouseButton] != 0);

        // ---- “reload pressed once” with hard re-arm (must release R) ----
        // This also prevents infinite reload if WM_KEYUP is lost.
        if (!w.keys[reloadKey]) reloadArmed = true;
        bool reloadPressed = reloadArmed && (w.keys[reloadKey] != 0);

        // ---- movement ----
        bool moving = (w.keys['W'] || w.keys['A'] || w.keys['S'] || w.keys['D']);

        // ---- init current clip ----
        if (currentClip.empty())
        {
            zoomActive = wantZoom;
            currentClip = pickLocomotionClip(inst, moving, zoomActive);
            inst.resetAnimationTime();
        }

        // =========================================================
        // PRIORITY 1) Reload: if pressed, force unzoom -> reload
        // =========================================================
        if (reloadPressed && has(reloadClip, inst))
        {
            reloadArmed = false;

            // Requirement: when zoom and R pressed -> back to unzoomed state, reload,
            // and after reload, zoom again if RMB still held.
            pendingZoomAfterReload = wantZoom; // remember intent
            zoomActive = false;                // force unzoom during reload

            action = Action::Reload;
            currentClip = reloadClip;
            inst.resetAnimationTime();

            fireAccumulator = 0.0f;
        }

        if (action == Action::Reload)
        {
            inst.update(currentClip, dt * reloadAnimRate);

            if (inst.animationFinished())
            {
                action = Action::None;

                // After reload: if RMB still held -> zoom back in (requirement)
                bool stillWantZoom = (w.mouseButtons[zoomMouseButton] != 0);
                zoomActive = pendingZoomAfterReload && stillWantZoom;
                pendingZoomAfterReload = false;

                currentClip = pickLocomotionClip(inst, moving, zoomActive);
                inst.resetAnimationTime();
            }
            return false;
        }

        // =========================================================
        // Zoom toggle (only when not reloading)
        // =========================================================
        zoomActive = wantZoom; // ADS is “hold RMB”

        // =========================================================
        // PRIORITY 2) Fire: continuous while LMB held
        // =========================================================
        bool fireDown = (w.mouseButtons[fireMouseButton] != 0);
        bool shotThisFrame = false;

        if (allowHoldFire && fireDown)
        {
            std::string fireUse = pickFireClip(inst, zoomActive);

            if (!fireUse.empty())
            {
                action = Action::Fire;
                fireAccumulator += dt;

                const float interval = (shotsPerSecond > 0.0f) ? (1.0f / shotsPerSecond) : 1e9f;
                while (fireAccumulator >= interval)
                {
                    fireAccumulator -= interval;
                    shotThisFrame = true;

                    // Force restart fire animation for each bullet
                    currentClip = fireUse;
                    inst.resetAnimationTime();
                }

                inst.update(currentClip, dt * fireAnimRate);
                return shotThisFrame;
            }
        }

        // not firing
        action = Action::None;
        fireAccumulator = 0.0f;

        // =========================================================
        // PRIORITY 3) Locomotion: idle/walk (and zoom variants)
        // =========================================================
        std::string desired = pickLocomotionClip(inst, moving, zoomActive);
        if (!desired.empty() && desired != currentClip)
        {
            currentClip = desired;
            inst.resetAnimationTime();
        }

        float rate = pickLocomotionRate(moving, zoomActive);
        inst.update(currentClip, dt * rate);

        // loop idle/walk/zoom idle/zoom walk
        if (inst.animationFinished())
            inst.resetAnimationTime();

        return false;
    }

    // Main用：每帧画枪前调用，自动给你正确的 gunX/Y/Z（开镜会变）
    void getGunOffset(float& outX, float& outY, float& outZ, float& RotY) const
    {
        if (zoomActive)
        {
			outX = zoomGunX; outY = zoomGunY; outZ = zoomGunZ; RotY = zoommodelRotY;
        }
        else
        {
			outX = gunX; outY = gunY; outZ = gunZ; RotY = modelRotY;
        }
    }

    bool isZooming() const { return zoomActive; }

private:
    enum class Action { None, Fire, Reload };

    Action action = Action::None;
    std::string currentClip;

    // fire timing
    float fireAccumulator = 0.0f;

    // reload gating
    bool reloadArmed = true;
    bool pendingZoomAfterReload = false;

    // zoom state
    bool zoomActive = false;

private:
    bool has(const std::string& name, AnimationInstance& inst)
    {
        return inst.animation && inst.animation->hasAnimation(name);
    }

    std::string pickFireClip(AnimationInstance& inst, bool zoom)
    {
        if (zoom && has(zoomFireClip, inst)) return zoomFireClip;
        if (has(fireClip, inst)) return fireClip;
        return "";
    }

    std::string pickLocomotionClip(AnimationInstance& inst, bool moving, bool zoom)
    {
        if (zoom)
        {
            if (moving && has(zoomWalkClip, inst)) return zoomWalkClip;
            if (!moving && has(zoomIdleClip, inst)) return zoomIdleClip;
            // fallback if zoom clips not present
        }

        if (moving && has(walkClip, inst)) return walkClip;
        if (!moving && has(idleClip, inst)) return idleClip;

        // final fallback
        if (inst.animation && !inst.animation->animations.empty())
            return inst.animation->animations.begin()->first;

        return "";
    }

    float pickLocomotionRate(bool moving, bool zoom) const
    {
        if (zoom) return moving ? zoomWalkRate : zoomIdleRate;
        return moving ? walkAnimRate : idleAnimRate;
    }
};
