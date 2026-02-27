# Descent 3 — 6DoF Movement Physics Reference

> [!NOTE]
> **Phase 3.5 implementation is complete.** Sections 9 (Implementation Path) and the CT_AI/CT_FLYING
> comparison have been removed — the thrust-based movement, afterburner, tri-chording, and
> `PLAYER_FLAGS` handling are all implemented in `BotApplyThrust()` / `BotConfigureAI()` in `bot.cpp`.
> This document is retained as a technical reference for the constants and packet flags it defines.

---

## 1. The Physics Model — Exponential Drag

Descent 3 uses an **exponential drag model** — velocity decays toward an equilibrium set by applied
thrust, with a time constant determined by mass and drag.

```
v_eq = force / drag         (terminal velocity under constant thrust)
τ    = mass / drag          (time constant — how fast v approaches v_eq)
v(t) = v_eq + (v₀ - v_eq) × exp(-t / τ)
```

When drag or mass is negligible, the engine falls back to simple Euler integration.

---

## 2. Thrust Construction — Tri-Chording

```cpp
// DoFlyingControl() — object.cpp:2424–2427
objp->mtype.phys_info.thrust =
    speed_scalar * Players[objp->id].movement_scalar *
    (
        (objp->orient.fvec * controls.forward_thrust  * phys_info.full_thrust) +
        (objp->orient.uvec * controls.vertical_thrust * phys_info.full_thrust) +
        (objp->orient.rvec * controls.sideways_thrust * phys_info.full_thrust)
    );
```

The three axes are added **without normalization** — tri-chording (all three axes simultaneously)
produces `√3 ≈ 1.732×` single-axis thrust. `BotApplyThrust()` mirrors this formula exactly.

### Speed Scalars

| Condition | speed_scalar |
|-----------|-------------|
| Normal | 1.0 |
| Outside terrain | 1.3 (30% boost) |
| `EF_FREEZE` effect | `effect_info->freeze_scalar` |

---

## 3. Afterburner — Thrust Multiplier Ramp (`object.cpp:2183–2195`)

```cpp
float punch_scalar;
if (Players[slot].afterburn_time_left > AFTERBURN_TIME * 0.90f)
    punch_scalar = 1.8f;
else if (Players[slot].afterburn_time_left > AFTERBURN_TIME * 0.80f) {
    float norm = (Players[slot].afterburn_time_left - AFTERBURN_TIME * 0.80f)
               / (AFTERBURN_TIME * 0.10f);
    punch_scalar = 1.0f + norm * 0.8f;  // lerps 1.0 → 1.8
} else {
    punch_scalar = 1.0f;
}
controls->forward_thrust = controls->afterburn_thrust * 1.6f * punch_scalar;
```

| Fuel remaining | punch_scalar | Total multiplier |
|----------------|-------------|-----------------|
| >90% | 1.8 | **2.88×** normal |
| 80–90% | 1.0–1.8 (lerp) | **1.6–2.88×** |
| <80% | 1.0 | **1.6×** |

`#define AFTERBURN_TIME 5.0f` — from `player_external.h:394`.

`BotApplyThrust()` replicates this ramp using `Bots[i].afterburner_fuel` instead of
`Players[slot].afterburn_time_left` (DoFlyingControl skips on dedicated server).

---

## 4. Multiplayer Packet Flags (`multi.cpp:1819–1826`)

These flags drive client-side visual effects in `MP_PLAYER_POS` packets.

| Flag | Value | Effect |
|------|-------|--------|
| `MPF_AFTERBURNER` | 1 | Blue afterburner flame |
| `MPF_OUTSIDE` | 2 | Player is in terrain |
| `MPF_DEAD` | 4 | Player is dead |
| `MPF_FIRED` | 8 | Weapon fired this frame |
| `MPF_SPRAY` | 16 | Spray weapon active |
| `MPF_ON_OFF` | 32 | On/off weapon active |
| `MPF_HEADLIGHT` | 64 | Headlight on |
| `MPF_THRUSTED` | 128 | Forward thrust → thruster plume |

Set from `PLAYER_FLAGS_*`:

| PLAYER_FLAGS | Value |
|-------------|-------|
| `PLAYER_FLAGS_AFTERBURN_ON` | `1 << 15` = 32768 |
| `PLAYER_FLAGS_THRUSTED` | `1 << 17` = 131072 |

Bots set these directly in `BotApplyThrust()`. Both must be set for clients to show
the afterburner flame effect.

---

## 5. `physics_info` Structure (`lib/object_external_struct.h:329–373`)

```cpp
struct physics_info {
    vector velocity;       // Current linear velocity (units/sec)
    vector thrust;         // Applied thrust force — integrated by PhysicsDoFrame if PF_USES_THRUST

    vector rotvel;         // Angular velocity (rad/sec)
    vector rotthrust;      // Angular thrust (rad/sec²)

    float  mass;           // Object mass
    float  drag;           // Linear drag coefficient
    float  rotdrag;        // Rotational drag coefficient
    float  full_thrust;    // Max thrust magnitude
    float  full_rotthrust; // Max rotational thrust

    uint32_t flags;        // PF_* physics flags
};
```

### Key Physics Flags

| Flag | Meaning |
|------|---------|
| `PF_USES_THRUST` | Thrust field integrated by `PhysicsDoFrame()` — must be set for bots |
| `PF_FIXED_VELOCITY` | Velocity is frozen — set by `ResetPlayerObject()` for non-local players; **must be cleared** in `BotConfigureAI()` |
| `PF_LEVELING` | Auto-level to horizon |

**Critical**: `PlayerSetControlToAI()` clears `PF_USES_THRUST` and sets `drag=0.1f`.
`BotConfigureAI()` immediately restores the ship template values and re-sets `PF_USES_THRUST`.

---

## 6. Quick Reference: Bot Physics Constants

From `BotConfigureAI()` / `BotCacheShipPhysics()` in `bot.cpp`:

```cpp
obj->ai_info->max_velocity       = 50.0f;   // orientation goal speed (units/sec)
obj->ai_info->max_delta_velocity = 0.0f;    // ZERO — prevents AI goals from writing velocity
obj->ai_info->max_turn_rate      = 16000;   // initial; overridden per-frame by BotApplyThrust
obj->ai_info->avoid_friends_distance = 40.0f; // must override after PlayerSetControlToAI (sets 0)

// Ship template values (restored after PlayerSetControlToAI):
obj->mtype.phys_info.mass          = Ships[ship_idx].phys_info.mass;
obj->mtype.phys_info.drag          = Ships[ship_idx].phys_info.drag;
obj->mtype.phys_info.rotdrag       = Ships[ship_idx].phys_info.rotdrag;
obj->mtype.phys_info.full_thrust   = Ships[ship_idx].phys_info.full_thrust;
obj->mtype.phys_info.full_rotthrust = Ships[ship_idx].phys_info.full_rotthrust;
obj->mtype.phys_info.flags &= ~PF_FIXED_VELOCITY;
obj->mtype.phys_info.flags |=  PF_USES_THRUST;
```
