# AI-Assisted Experimental Avionics Laboratory

Version: 0.2  
Author: Carl Suttle  
Date: 2026

## 1. Purpose

The purpose of this laboratory is to explore pilot-centric instrumentation using low-cost sensors and AI-assisted engineering workflows.

This project emerged from the observation that AI-assisted development
dramatically reduces the effort required to implement avionics
infrastructure.

Historically, avionics prototyping required significant engineering
resources. With AI assistance, a technically minded pilot can now
implement telemetry, sensor fusion scaffolding, and display logic
rapidly.

This project explores whether that shift enables wider experimentation
with flight instrumentation and pilot-centric displays.

The central question is:

Can a technically minded pilot, working with AI tools, design and prototype useful flight instrumentation systems that historically required larger specialist engineering teams?

This is an experimental and research-oriented effort, not a certified avionics program.

Primary aims:

- Explore pilot-centric instrumentation
- Accelerate avionics prototyping
- Evaluate AI as an engineering collaborator
- Document a repeatable AI-assisted development method
- Build a transferable knowledge base for future AI sessions and future related projects

Working hypothesis:

AI reduces implementation friction enough that domain expertise, test discipline, and system judgement become the main drivers of useful innovation.

## 2. Role Of This Document

This document serves four purposes:

- Project memory
- Architecture reference
- AI session briefing
- Laboratory notebook summary

One explicit purpose is continuity across AI sessions. It should allow a new AI chat or a related future project to inherit the current architectural understanding, constraints, intent, and known lessons without restarting from zero.

## 3. Project Philosophy

### 3.1 Domain Knowledge Drives Design

Aviation knowledge defines the problems worth solving and the behaviors the system should exhibit.

AI assists with implementation, refactoring, debugging, scaffolding, and documentation. AI does not define avionics meaning, pilot needs, or validation criteria.

### 3.2 Rapid Prototyping

The working loop is:

1. Identify an instrumentation problem
2. Form a concept
3. Implement rapidly with AI assistance
4. Bench test
5. Simulate or flight test when appropriate
6. Observe and document
7. Iterate

AI most strongly accelerates step 3.

### 3.3 Demonstration Over Perfection

The immediate goal is not polished productization. The key milestone is the first working demonstration of an idea.

A first successful demonstration is treated as proof of feasibility and a basis for refinement.

### 3.4 Pilot-Centric Instrumentation

The project prioritizes pilot situational awareness over sensor novelty or automation for its own sake.

Core questions include:

- What information does a pilot actually need?
- When do conventional instruments mislead?
- How can energy state be represented more clearly?
- Which derived quantities are more useful than raw sensor outputs?

## 4. Engineering Method

This work follows a model that can be described as:

**AI-Assisted Domain Engineering**

Traditional path:

Idea -> engineering organization -> implementation -> prototype

This project path:

Domain insight -> AI collaboration -> prototype

As implementation becomes easier, the hard parts shift toward:

- System architecture
- Validation
- Test design
- Human factors
- Interpretation of physical behavior

## 5. Current System Architecture

### 5.1 High-Level Structure

The system is currently organized into three main processing roles:

- `Teensy 4.0`
- `ESP_AIR` on ESP32-S3
- `ESP_GND` on ESP32

### 5.2 Teensy 4.0 Flight Computer

Primary responsibilities:

- Sensor acquisition
- Sensor fusion
- Flight state estimation
- Core avionics logic
- Fast deterministic processing

### 5.3 Sensors

Current sensors in the development system include:

- IMU: BMI270
- Magnetometer: BMM150
- GPS: u-blox M10 using NAV-PVT
- Barometer: BMP280

### 5.4 ESP_AIR

Primary responsibilities:

- Receive telemetry/state from the Teensy over UART
- Radio and networking transport
- Logging infrastructure
- Support for instrumentation data export and development diagnostics

### 5.5 ESP_GND

Primary responsibilities:

- Ground-side networking and web interface
- Instrumentation display
- Telemetry reception and presentation
- Ground-side user interaction

### 5.6 Data Path

Current logical path:

Sensors -> Teensy fusion/state estimation -> UART -> ESP_AIR -> wireless transport -> ESP_GND -> web instrumentation/display

### 5.7 Approximate Update Rates

- IMU processing: about 400 Hz
- GPS: 10 Hz
- Telemetry transport: about 20 Hz
- Display refresh: about 20 Hz

These rates are approximate and may change during testing.

## 6. Current Development State

### 6.1 Working Or Partially Working Components

- GPS NAV-PVT parsing
- IMU fusion running at approximately 400 Hz
- Telemetry state structures
- UART transport between Teensy and ESP processors
- Ground/web instrumentation architecture in active development

### 6.2 Active Problem Areas

- Heading and magnetic frame consistency
- Roll/pitch-induced heading instability investigation
- Acceleration rejection behavior during certain maneuvers
- SD logging integration and write-path performance validation

### 6.3 Current Development Character

The codebase is in active bench-development mode. Some subsystems are functional, but architecture and validation are still evolving.

## 7. Key Technical Insights So Far

### 7.1 GPS Velocity Is Extremely Valuable

GPS velocity appears to be one of the most useful external references in the system.

It is valuable for:

- Heading validation
- Ground track estimation
- Energy-related calculations
- Cross-checking inertial outputs

### 7.2 Gravity-Vector Assumptions Break In Turns

Many attitude solutions behave best when measured acceleration is dominated by gravity.

In coordinated turns, centripetal acceleration violates that assumption and can corrupt attitude interpretation if treated naively.

Promising supporting inputs include:

- Yaw rate
- GPS velocity
- Turn geometry

### 7.3 Instrumentation Should Emphasize Meaning

Sensors provide numbers. Pilots need understanding.

A useful instrument should communicate flight state and operational meaning, not simply expose raw sensor values.

### 7.4 AI Is Strong On Implementation, Weak On Domain Judgement

AI tools are highly effective at:

- Infrastructure code
- Communication protocols
- Software scaffolding
- Debug pattern generation
- Refactoring and iteration support

AI tools are weaker at:

- System architecture
- Aviation physics
- Operational judgement
- Validation of physical assumptions
- Knowing when a "working" implementation is misleading

Therefore human domain judgement remains primary.

## 8. Constraints And Design Rules

### 8.1 Flight-Critical Processing First

Deterministic flight-computer processing must not depend on networking performance.

### 8.2 Logging Must Be Non-Blocking

Logging is important, but it must not interfere with core avionics timing.

Better wording than "must never block or fail" is:

- Logging must be designed to be non-blocking with bounded loss behavior
- Logging must not degrade primary flight-state processing
- Networking is secondary to state estimation

### 8.3 Debug Visibility Must Be Preserved

The system must remain inspectable through serial diagnostics and explicit internal status reporting.

### 8.4 Experimental Discipline

Because the project is exploratory, each significant change should ideally be testable in isolation and documented with observed behavior.

## 9. Validation Approach

This project should be validated progressively:

### 9.1 Bench Validation

- Static sensor checks
- Calibration verification
- Synthetic input tests where possible
- Transport and logging stress tests

### 9.2 Simulation Or Controlled Dynamic Tests

- Repeatable motion tests
- Injected reference vectors
- Tilt, roll, pitch, and heading consistency checks

### 9.3 Flight Test

Flight test is only appropriate after bench confidence is sufficient for the specific function being explored.

### 9.4 Validation Principle

A result should not be accepted only because it "looks right." Wherever possible, it should be compared against an explicit physical expectation or an independent reference.

## 10. Known Risks And Open Technical Questions

Known or suspected issues include:

- Magnetometer frame/sign ambiguity
- Heading instability under roll and pitch
- Acceleration rejection behavior during maneuvering
- Logging interference with transport timing
- Over-trusting fused outputs without validating frame assumptions

Open questions include:

- How best to estimate attitude in coordinated turns?
- How much can GPS support inertial validation?
- Which pilot displays are most informative for energy awareness?
- Which derived flight-state representations are operationally useful rather than merely mathematically interesting?

## 11. Next Experiments

Near-term experiments may include:

- Improved roll estimation in coordinated turns
- GPS-assisted validation of inertial estimates
- Energy-state display concepts
- Alternative attitude presentation methods
- Telemetry-driven cockpit displays
- Buffered SD logging performance tests

## 12. Broader Insight

One of the most important emerging insights is that AI may materially lower the barrier to experimental avionics prototyping.

Historically, many instrumentation ideas were inaccessible because implementation overhead was too high. If AI removes enough of that overhead, more experimentation may become possible by individuals with domain knowledge but without large engineering teams.

Potential consequences:

- More instrumentation experiments
- More domain-led innovation
- Faster prototype cycles
- Wider participation in avionics idea generation
- Possible safety improvements through better pilot-focused displays

## 13. AI Session Briefing

The following section is intended to be copied into future AI sessions when continuity matters.

### 13.1 Short Briefing

You are assisting an experimental avionics research project.

Architecture:

- Teensy 4.0 flight computer
- `ESP_AIR` telemetry/logging processor
- `ESP_GND` ground/web interface processor
- Sensors include IMU, magnetometer, GPS, and barometer

Goals:

- Explore pilot-centric instrumentation
- Prototype new ways of presenting flight state
- Preserve deterministic processing while expanding telemetry and logging capability

Constraints:

- Flight-state processing is primary
- Logging must be non-blocking
- Networking is secondary
- System must remain debuggable
- Do not assume sensor-frame conventions are correct without verification

Working style:

- Move quickly
- Prefer bench-verifiable changes
- Keep architecture explicit
- Treat physical interpretation separately from code implementation

### 13.2 Extended AI Guidance

When assisting this project, AI should:

- Prioritize architectural clarity over ad hoc fixes
- Preserve observability and debugging paths
- Avoid "looks right" coordinate transforms without explicit justification
- Distinguish between confirmed behavior and inferred behavior
- Support repeatable tests and documentation
- Treat this document as project context, not as proof that any subsystem is already correct

## 14. Record Maintenance Guidance

This file should be updated when any of the following change materially:

- High-level architecture
- Major lessons learned
- Core constraints
- Active risks
- Validation philosophy
- AI workflow assumptions

Detailed dated experiment logs may later be maintained in a separate decision log or lab log file, with this file remaining the stable high-level record.
