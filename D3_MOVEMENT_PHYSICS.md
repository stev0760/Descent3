# Descent 3 — 6DoF Movement Physics Reference

This document covers Descent 3's movement physics in detail, including the exact formulas used
in the engine, multiplayer packet flags, and the gap between how human players and CT_AI bots
move. Its purpose is to guide the Phase 3.5 implementation of CT_FLYING synthetic controls for
bots so they move with the same physical fidelity as human players.

---

## 1. The Physics Model

Descent 3 does **not** use simple Euler integration. It uses an **exponential drag model** —
velocity decays toward an equilibrium set by the applied thrust, with a time constant
determined by mass and drag.

### Core Integration (`physics/physics.cpp:260–293`)

```cpp
// PhysicsApplyConstantForce() — called every frame by PhysicsDoFrame()
const double dt          = deltaTime;
const double massOverDrag    = mass / drag;
const double forceOverDrag[3] = { force.x/drag, force.y/drag, force.z/drag };
const double expTerm     = exp((-1.0 / massOverDrag) * dt);

// New velocity
newVel = (vel - forceOverDrag) * expTerm + forceOverDrag;

// New position
newPos = pos + forceOverDrag * dt
             + massOverDrag * (vel - forceOverDrag) * (1.0 - expTerm);
```

When drag or mass is negligible, it falls back to simple Euler:

```cpp
// Low-drag fallback (physics.cpp:267–272)
movementVec = vel * deltaTime + force * (0.5 * deltaTime * deltaTime);
newPos = pos + movementVec;
newVel = vel + force * deltaTime;
```

### Derived Formulas

**Equilibrium velocity** (terminal velocity under constant thrust):
```
v_eq = force / drag = (full_thrust × control_input) / drag
```

**Decay time constant** (how quickly velocity approaches equilibrium):
```
τ = mass / drag
```

**Velocity at time t after thrust is applied**:
```
v(t) = v_eq + (v₀ - v_eq) × exp(-t / τ)
```

The practical result: when a player pushes forward, speed climbs toward `v_eq`
asymptotically rather than instantly. Release the thrust and it decays back toward
zero with the same exponential curve. **This is the momentum/inertia players feel.**

CT_AI bots bypass this entirely by writing `phys_info.velocity` directly each frame
(see §7). They have no inertia.

---

## 2. Thrust Construction — Tri-Chording (`object.cpp:2265–2445`)

### The Thrust Formula (`object.cpp:2424–2427`)

```cpp
// DoFlyingControl() — runs every frame for CT_FLYING objects
objp->mtype.phys_info.thrust =
    speed_scalar * Players[objp->id].movement_scalar *
    (
        (objp->orient.fvec * controls.forward_thrust  * phys_info.full_thrust) +
        (objp->orient.uvec * controls.vertical_thrust * phys_info.full_thrust) +
        (objp->orient.rvec * controls.sideways_thrust * phys_info.full_thrust)
    );
```

Each control axis is a float in `[-1.0, 1.0]`. The three thrust components are added
as raw vectors with **no normalization**. This is intentional and is the source of
tri-chording.

### The Tri-Chord Math

When a player simultaneously inputs full forward + full strafe + full vertical:

```
|thrust| = full_thrust × √(1² + 1² + 1²) = full_thrust × √3 ≈ 1.732 × single-axis
```

At equilibrium this means top speed is `√3 ≈ 1.732×` the single-axis top speed.
D3 deliberately does not normalize the input vector — unlike Unity/Unreal which cap
diagonal movement to 1.0. **This is the most important mechanical difference for bots.**

### Speed Scalars (`object.cpp:2412–2422`)

| Condition | speed_scalar |
|-----------|-------------|
| Normal | 1.0 |
| Outside terrain | 1.3 (30% boost) |
| `EF_FREEZE` effect | `effect_info->freeze_scalar` |

The `movement_scalar` is a per-player modifier (default 1.0; modified by powerups).

### `PLAYER_FLAGS_THRUSTED` (`object.cpp:2387, 2416`)

The flag is reset at the start of `DoFlyingControl()` and set if `forward_thrust > 0`:

```cpp
Players[slot].flags &= ~PLAYER_FLAGS_THRUSTED;  // reset each frame
// ...
if (controls.forward_thrust > 0)
    Players[slot].flags |= PLAYER_FLAGS_THRUSTED;
```

Note: only **forward** thrust sets `PLAYER_FLAGS_THRUSTED` — lateral/vertical
movement alone does not trigger it. The MPF packet flag for thruster glow therefore
only fires on forward thrust for humans (see §5).

---

## 3. Afterburner (`object.cpp:2169–2226`)

### Activation and Fuel (`player_external.h:394`)

```cpp
#define AFTERBURN_TIME 5.0f   // seconds of fuel at full burn
```

Fuel drains during afterburner use and recharges when idle. A powerup doubles the
recharge rate.

### Thrust Multiplier (`object.cpp:2183–2195`)

The multiplier is not a flat value — it ramps based on remaining fuel:

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
| >90% | 1.8 | **2.88×** normal thrust |
| 80–90% | 1.0–1.8 (lerp) | **1.6–2.88×** |
| <80% | 1.0 | **1.6×** |

### Flags Set During Afterburn (`object.cpp:2197`)

```cpp
Players[slot].flags |= PLAYER_FLAGS_AFTERBURN_ON | PLAYER_FLAGS_THRUSTED;
```

Both flags are set simultaneously. This drives `MPF_AFTERBURNER` and `MPF_THRUSTED`
in position packets, producing the visible blue flame effect on all clients.

Flag values (from `player_external.h`):

| Flag | Value |
|------|-------|
| `PLAYER_FLAGS_AFTERBURN_ON` | `1 << 15` = 32768 |
| `PLAYER_FLAGS_THRUSTED` | `1 << 17` = 131072 |

---

## 4. Rotation (`object.cpp:2404–2409`)

Rotation uses the same thrust-and-drag model as translation but acts on angular velocity:

```cpp
objp->mtype.phys_info.rotthrust.x() =
    controls.pitch_thrust * phys_info.full_rotthrust * playp->turn_scalar;
objp->mtype.phys_info.rotthrust.z() =
    controls.bank_thrust  * phys_info.full_rotthrust * playp->turn_scalar;
objp->mtype.phys_info.rotthrust.y() =
    controls.heading_thrust * phys_info.full_rotthrust * playp->turn_scalar;
```

`rotdrag` damps angular velocity. `full_rotthrust` and `rotdrag` are ship-specific.
Ship turn rates vary significantly:
- Fast ships (Phoenix-class): ~150°/s max
- Heavy ships (Magnum-class): ~70–90°/s max

---

## 5. Multiplayer Packet Flags (`multi.cpp:1819–1965`)

Position packets (`MP_PLAYER_POS`) include a flags byte that clients use to drive
visual effects (thruster glow, afterburner flames, weapon fire).

### Flag Definitions (`multi.cpp:1819–1826`)

```cpp
#define MPF_AFTERBURNER   1    // Afterburner is on → blue flame effect
#define MPF_OUTSIDE       2    // Player is in terrain
#define MPF_DEAD          4    // Player is dead
#define MPF_FIRED         8    // Player fired a weapon this frame
#define MPF_SPRAY         16   // Spray weapon active
#define MPF_ON_OFF        32   // On/off weapon active
#define MPF_HEADLIGHT     64   // Headlight on
#define MPF_THRUSTED      128  // Forward thrust active → thruster plume
```

### How Flags Are Set in `MultiStuffPosition()` (`multi.cpp:1922–1932`)

```cpp
// Human players: driven by PLAYER_FLAGS_* from physics
if (Players[slot].flags & PLAYER_FLAGS_AFTERBURN_ON)
    flags |= MPF_AFTERBURNER;
if (Players[slot].flags & PLAYER_FLAGS_THRUSTED)
    flags |= MPF_THRUSTED;

// Bot workaround (current Phase Mov implementation):
// PLAYER_FLAGS_THRUSTED is never set by CT_AI, so proxy via velocity
if (BotIsPlayerSlot(slot) && !(flags & MPF_THRUSTED)) {
    if (vm_GetMagnitude(&obj->mtype.phys_info.velocity) > 1.0f)
        flags |= MPF_THRUSTED;
}
```

The bot velocity proxy works for the thruster glow effect but does not produce the
afterburner flame (`MPF_AFTERBURNER` stays off since `PLAYER_FLAGS_AFTERBURN_ON` is
never set by CT_AI). With CT_FLYING synthetic controls, both flags would be set
automatically by the real engine path.

### Velocity Encoding (`multi.cpp:1942–1948`)

Velocity is compressed to 16-bit integers with a 128× scale factor:

```cpp
MultiAddShort((int16_t)(vel->x() * 128.0f), data, &count);
MultiAddShort((int16_t)(vel->y() * 128.0f), data, &count);
MultiAddShort((int16_t)(vel->z() * 128.0f), data, &count);
```

Max representable velocity: 32767 / 128 ≈ **256 units/sec** — well above any
achievable speed, so no clamping occurs in practice.

---

## 6. `physics_info` Structure (`lib/object_external_struct.h:329–373`)

```cpp
struct physics_info {
    vector velocity;           // Current linear velocity (units/sec)
    vector thrust;             // Applied thrust force (units/sec²)

    vector rotvel;             // Angular velocity (rad/sec)
    vector rotthrust;          // Angular thrust (rad/sec²)

    angle  turnroll;           // Banking angle from turning
    float  last_still_time;    // For wiggle amplitude
    int32_t num_bounces;

    float  coeff_restitution;  // Bounce elasticity (0=inelastic, 1=elastic)
    float  mass;               // Object mass
    float  drag;               // Linear drag coefficient
    float  rotdrag;            // Rotational drag coefficient

    float  full_thrust;        // Max thrust magnitude    [or max_velocity in AI mode]
    float  full_rotthrust;     // Max rotational thrust   [or max_turn_rate in AI mode]

    float  max_turnroll_rate;
    float  turnroll_ratio;
    float  wiggle_amplitude;
    float  wiggles_per_sec;

    vector dest_pos;           // Multiplayer interpolation target position

    uint32_t flags;            // PF_* physics flags
};
```

### Key Physics Flags

| Flag | Meaning |
|------|---------|
| `PF_USES_THRUST` | Thrust field included in physics integration |
| `PF_LEVELING` | Auto-level to horizon |
| `PF_GRAVITY` | Apply gravity |
| `PF_WIGGLE` | Enable oscillation |
| `PF_POINT_COLLIDE_WALLS` | Point collision instead of sphere |

**CT_AI clears `PF_USES_THRUST`** (`Player.cpp:2603`): the thrust field is ignored
and velocity is written directly by the AI goal system. CT_FLYING sets `PF_USES_THRUST`
and relies on `PhysicsDoFrame()` to integrate thrust into velocity.

---

## 7. CT_AI vs CT_FLYING — The Core Difference

### `PlayerSetControlToAI()` (`Player.cpp:2595–2644`)

```cpp
void PlayerSetControlToAI(int slot, float velocity) {
    object *pobj = &Objects[Players[slot].objnum];

    pobj->mtype.phys_info.flags &= ~PF_USES_THRUST;  // Disable thrust physics
    pobj->mtype.phys_info.drag = 0.1f;               // Very low drag
    pobj->mtype.phys_info.flags &= ~PF_LEVELING;

    vm_MakeZero(&pobj->mtype.phys_info.thrust);
    vm_MakeZero(&pobj->mtype.phys_info.rotthrust);
    vm_MakeZero(&pobj->mtype.phys_info.rotvel);
    vm_MakeZero(&pobj->mtype.phys_info.velocity);

    SetObjectControlType(pobj, CT_AI);

    pobj->ai_info->max_velocity      = velocity;   // Hard speed cap
    pobj->ai_info->max_delta_velocity = 40.0f;     // Max velocity change/frame
    pobj->ai_info->max_turn_rate     = 14000;
    pobj->ai_info->movement_type     = MC_FLYING;
}
```

### Comparison Table

| Property | Human (CT_FLYING) | Bot (CT_AI) |
|----------|-------------------|-------------|
| Velocity source | `PhysicsDoFrame()` integrates thrust | AI goal writes velocity directly |
| Inertia | Yes — exponential approach to v_eq | None — instant direction change |
| Drag model | `exp(-t/τ)` decay | `drag = 0.1f`, effectively no drag |
| Afterburner | Real: `full_thrust × 2.88×`, PLAYER_FLAGS set | Unavailable |
| Tri-chording | Yes: thrust = fvec + uvec + rvec (raw add) | No: AI picks single direction |
| Terrain speed bonus | Yes: `speed_scalar *= 1.3` | Yes: inherited via same physics call |
| PLAYER_FLAGS_THRUSTED | Set by engine on forward thrust | Never set (requires MPF hack) |
| PLAYER_FLAGS_AFTERBURN_ON | Set by engine on afterburn | Never set |
| Top speed (no AB) | ~50 units/sec (equilibrium) | 50 units/sec (hard cap) |
| Top speed (afterburner, full tank) | ~144 units/sec (2.88× × 50) | Unavailable |
| Top speed (tri-chord) | ~87 units/sec (√3 × 50) | Unavailable |

---

## 8. Bot Tactical Implications

### Tri-Chording for Bot Navigation

A competitive bot should not simply drive its velocity vector at the target. It should
decompose movement into axes:

- **Forward axis** (`fvec`): points toward target — 1.0 thrust
- **Sideways axis** (`rvec`): oscillates ±1.0 on a timer — adds lateral evasion
- **Vertical axis** (`uvec`): can add ±0.5 — adds vertical unpredictability

Combined, the bot travels at up to `√3` speed while remaining harder to track. The
"nose" (weapon forward) stays pointed at the target regardless of the velocity vector
direction — this is the key separation between heading and velocity in 6DoF.

### Afterburner Usage Strategy

Human players use afterburner tactically:
- **Gap closing**: entering weapon range from a distance
- **Breaking LOS**: escaping a circle-strafe that has gone badly
- **Flee boost**: escaping a losing engagement

A bot simulating afterburner (via a temporary velocity cap boost and the `MPF_AFTERBURNER`
flag) should follow the same decision logic: afterburner in HUNT state (gap closing) or
FLEE state (escape), conserve in COMBAT (circle-strafing at close range).

With CT_FLYING synthetic controls, afterburner would be driven by the real engine path
(`DoPlayerAfterburnControl`) and fuel management would be automatic.

### The "Spiral Approach" Pattern

Instead of a straight-line pursuit (current beeline behavior), a bot with lateral
thrust available can compute a spiral intercept:

```
Every N frames:
  to_target = normalize(target.pos - bot.pos)
  perp       = cross(to_target, up_vector)  // perpendicular in horizontal plane
  juke_phase += delta_time * juke_frequency // e.g. 0.5 Hz
  lateral    = perp * sin(juke_phase) * juke_amplitude

  controls.forward_thrust  = 1.0
  controls.sideways_thrust = lateral component
  controls.vertical_thrust = small oscillation
```

This produces the characteristic Descent dodging weave that human players use. The
weapon aim point is computed separately from the movement direction.

---

## 9. Phase 3.5 Implementation Path

The goal is to replace `CT_AI` with `CT_FLYING` for bots and drive movement via
synthetic control inputs rather than directly writing velocity.

### Step 1: Switch Bot Objects to CT_FLYING

Instead of calling `PlayerSetControlToAI()`, call the equivalent CT_FLYING setup:

```cpp
// Clear AI control structures
pobj->mtype.phys_info.flags |= PF_USES_THRUST;  // Enable thrust physics
SetObjectControlType(pobj, CT_FLYING);
// Restore ship phys_info from Ships[ship_index].phys_info template
// (mass, drag, full_thrust, full_rotthrust, etc.)
```

### Step 2: Per-Frame Synthetic Controls

Each bot frame (in `BotDoFrame()`), compute a `game_controls`-equivalent struct
based on FSM state and feed it into the bot's physics:

| Bot State | Forward | Sideways | Vertical | Afterburner |
|-----------|---------|----------|----------|-------------|
| WANDER | 0.5 (wander dir) | oscillate | oscillate | off |
| HUNT | 1.0 (to target) | ±juke | ±juke | if gap > 2× fire range |
| COMBAT | 0.3–0.7 (orbit) | 1.0 (strafe) | ±0.3 | off |
| FLEE | 1.0 (away) | ±0.5 | ±0.3 | on |

Then call the physics setup instead of writing velocity directly:

```cpp
// Compute thrust vector
vector desired_thrust =
    obj->orient.fvec * controls.forward_thrust   * phys_info.full_thrust +
    obj->orient.uvec * controls.vertical_thrust  * phys_info.full_thrust +
    obj->orient.rvec * controls.sideways_thrust  * phys_info.full_thrust;

obj->mtype.phys_info.thrust = desired_thrust * speed_scalar * movement_scalar;
// PhysicsDoFrame() integrates this into velocity each frame automatically
```

### Step 3: Rotation Control

Drive `phys_info.rotthrust` toward the target instead of using `ai_info->max_turn_rate`:

```cpp
// Compute desired heading toward target/aim point
// Convert to pitch/yaw/roll control inputs
// Write to phys_info.rotthrust (same formula as player rotation, §4)
```

### Key Files for Implementation

| Concept | File | Lines |
|---------|------|-------|
| Afterburner thrust math | `Descent3/object.cpp` | 2169–2226 |
| Tri-chord thrust combination | `Descent3/object.cpp` | 2387–2427 |
| Exponential drag integration | `physics/physics.cpp` | 260–293 |
| CT_AI physics setup | `Descent3/Player.cpp` | 2595–2644 |
| MPF packet flags | `Descent3/multi.cpp` | 1819–1965 |
| `physics_info` struct | `lib/object_external_struct.h` | 329–373 |
| PLAYER_FLAGS constants | `Descent3/player_external.h` | 79–99 |
| AFTERBURN_TIME constant | `Descent3/player_external.h` | 394 |
| Ship physics template | `Descent3/ship.h` | 145–174 |

---

## 10. Quick Reference: Current Bot Physics Constants

From `BotConfigureAI()` in `bot.cpp`:

```cpp
obj->ai_info->max_velocity      = 50.0f;   // hard speed cap (units/sec)
obj->ai_info->max_delta_velocity = 40.0f;  // max acceleration (units/sec per frame)
obj->ai_info->max_turn_rate     = 16000;   // rotation rate
pobj->mtype.phys_info.drag      = 0.1f;   // very low drag (set by PlayerSetControlToAI)
```

These are the primary tuning knobs for CT_AI mode. In CT_FLYING mode they would be
replaced by `full_thrust`, `drag`, `mass`, and `full_rotthrust` from the ship template.
