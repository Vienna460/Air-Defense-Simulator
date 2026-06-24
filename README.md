# Air Defense Simulation

A simple real-time air defense command simulation built with C++ and SDL2.

This project simulates a basic radar defense system capable of:

* detecting airborne threats
* tracking and classifying targets
* assigning weapons
* launching interceptors
* assessing kills in real time

The goal of the project was to recreate the feel of a tactical radar operations screen while keeping the code readable and lightweight.

---

# Features

## Radar Simulation

* Rotating radar sweep
* Real-time target detection
* Contact visibility based on sweep position

## Threat System

Randomly generated airborne threats including:

* Fighter Jets
* Cruise Missiles
* Ballistic Missiles
* UAV Drones

Each threat has:

* movement behavior
* speed
* priority level
* tracking state

## Kill Chain Workflow

Targets move through a full engagement pipeline:

```text
SENSE → DETECT → TRACK → CLASSIFY → EVALUATE → ASSIGN → ENGAGE → ASSESS
```

This mimics a simplified real-world air defense process.

## Weapon Systems

Includes multiple defensive systems:

* SAM batteries
* CIWS systems

Weapons are automatically assigned based on target distance and engagement logic.

## Interceptor Simulation

* Missile launch and tracking
* Homing interceptor behavior
* Hit detection
* Explosion effects

## Tactical UI

* Live combat log
* Threat selection panel
* Target trails
* Radar effects
* Visual engagement indicators

---

# Technologies Used

* C++
* SDL2
* SDL2_ttf

---

# Controls

| Key              | Action                    |
| ---------------- | ------------------------- |
| `T`              | Spawn random threat       |
| `SPACE`          | Pause / Resume simulation |
| Left Mouse Click | Select target             |
| `ESC`            | Exit application          |

---

# Building the Project

## Requirements

Install:

* SDL2
* SDL2_ttf
* g++ compiler

---

## Windows (MSYS2)

Install dependencies:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
pacman -S mingw-w64-ucrt-x86_64-SDL2
pacman -S mingw-w64-ucrt-x86_64-SDL2_ttf
```

Compile:

```bash
g++ air_defense.cpp -o air_defense $(sdl2-config --cflags --libs) -lSDL2_ttf
```

Run:

```bash
./air_defense.exe
```

---

# Project Structure

The project currently exists as a single-file application containing:

* rendering logic
* simulation systems
* entity management
* UI systems

Main systems include:

* threat management
* interceptor logic
* radar rendering
* combat logging
* stage progression

---

# How It Works

## Threat Lifecycle

Threats spawn outside radar range and move toward the defended asset at the center of the radar.

The radar sweep detects threats only when the sweep line passes over them.

Once detected, targets move through several processing stages:

1. Detection
2. Tracking
3. Classification
4. Threat evaluation
5. Weapon assignment
6. Engagement
7. Kill assessment

If an interceptor successfully reaches the target, the threat is destroyed and logged.

---

# Future Improvements

Planned improvements may include:

* multiple radar stations
* smarter interceptor AI
* different missile behaviors
* map system
* multiplayer support
* advanced targeting logic
* better UI scaling
* sound effects
* OpenGL rendering

---

# Screenshots

Add screenshots here once available.

Example:

```markdown
![Radar Screen](screenshots/radar.png)
```

---

# Purpose of the Project

This project was made as a practice and learning project focused on:

* simulation programming
* SDL2 rendering
* real-time systems
* game-loop architecture
* tactical UI design

It also served as an experiment in building a lightweight command-and-control style application entirely in C++.

---

# License

This project is open-source and free to modify for educational or personal use.
