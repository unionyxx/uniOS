#pragma once

// ============================================================================
// uniOS Version Information
// ============================================================================
//
// Format: 0.MAJOR.MINOR (pre-1.0), then MAJOR.MINOR.PATCH (post-1.0)
//
// --- Pre-1.0 Rules (Current Development Phase) ---
//
// Increment MAJOR (0.X.0) when:
//   - Complete implementation of a core kernel subsystem
//   - Add fundamental OS capability that didn't exist before
//   - Make architectural changes that affect multiple components
//   - Achieve a significant development milestone
//
// Increment MINOR (0.X.Y) when:
//   - Add new features within existing subsystems
//   - Implement new commands, utilities, or drivers
//   - Make non-breaking improvements to functionality
//   - Fix bugs that significantly affect user experience
//   - Add hardware support or compatibility fixes
//
// Don't increment for:
//   - Documentation changes
//   - Code refactoring without behavioral changes
//   - Build system modifications
//   - Minor code cleanup
//
// --- Post-1.0 Rules (Stable Release) ---
//
// Increment MAJOR (X.0.0) when:
//   - Breaking changes to kernel ABI or system calls
//   - Changes to filesystem format requiring migration
//   - Modifications to boot protocol or kernel interface
//   - Any change that breaks backward compatibility
//
// Increment MINOR (X.Y.0) when:
//   - New subsystems or major features (backward compatible)
//   - New system calls or APIs
//   - Performance improvements
//   - New hardware support
//
// Increment PATCH (X.Y.Z) when:
//   - Bug fixes only
//   - Security patches
//   - Minor optimizations
//   - No API or feature changes
//
// ============================================================================

#define UNIOS_VERSION_MAJOR 0
#define UNIOS_VERSION_MINOR 5
#define UNIOS_VERSION_PATCH 1

#define UNIOS_VERSION_STRING "0.5.2"
#define UNIOS_VERSION_FULL   "uniOS v0.5.2"

// Build date (set at compile time)
#define UNIOS_BUILD_DATE __DATE__
