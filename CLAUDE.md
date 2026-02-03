# CLAUDE.md - Field Compass Project Guidelines

## Project Overview

**Hardware Platform:** Adafruit ESP32-S3 Feather with 4MB Flash 2MB PSRAM - STEMMA QT / Qwiic
**Project Type:** Embedded firmware development with FeatherWing and I2C peripherals

## Hardware Specifications

- **MCU:** ESP32-S3 (dual-core Xtensa LX7)
- **Flash:** 4MB
- **PSRAM:** 2MB
- **Connectivity:** STEMMA QT / Qwiic (I2C)
- **Form Factor:** Adafruit Feather

## Development Workflow Requirements

### Backlog Management (GitHub Issues)

All backlog items MUST be created as GitHub Issues with the following **mandatory fields**:

| Field | Description | Required |
|-------|-------------|----------|
| **Title** | Clear, concise description of the item | Yes |
| **Body** | Detailed description including context | Yes |
| **Status** | Current state (see Status Labels below) | Yes |
| **Priority** | Importance level (see Priority Labels below) | Yes |

#### Status (GitHub Project Field)
- `Todo` - Not yet started
- `In Progress` - Currently being worked on
- `Testing` - Ready for review/testing
- `Done` - Completed

#### Priority (GitHub Project Field)
- `P0 - Critical` - Must be addressed immediately
- `P1 - High` - Important, address soon
- `P2 - Medium` - Normal priority
- `P3 - Low/Cosmetic` - Nice to have, cosmetic issues
- `P4 - Deferred` - Postponed for future consideration

#### Issue Types
- `type:enhancement` - New feature or improvement
- `type:bug` - Defect or unexpected behavior
- `type:documentation` - Documentation updates
- `type:refactor` - Code improvement without behavior change

### Issue Lifecycle

1. **Creation:** Automatically create Issues when enhancements, bugs, or documentation needs are identified
2. **Updates:** Add comments to Issues for each significant update including:
   - Progress notes
   - Decisions made
   - Blockers encountered
3. **Specifications:** Document technical specifications in the Issue body
4. **Acceptance Criteria:** Define clear, testable acceptance criteria
5. **Completion:** Update Status to `Done` when all acceptance criteria are met

### Version Control

#### Commit Strategy
- Commit each **successfully compiled** version
- Write clear, descriptive commit messages
- Reference related Issue numbers in commits (e.g., `Fixes #12`)

#### Version Tagging (Semantic Versioning)

Format: `vMAJOR.MINOR.PATCH`

| Component | When to Increment |
|-----------|------------------|
| **MAJOR** | Breaking changes, incompatible API changes |
| **MINOR** | New features, backward-compatible additions |
| **PATCH** | Bug fixes, backward-compatible fixes |

**Examples:**
- `v0.1.0` - Initial development release
- `v1.0.0` - First stable release
- `v1.1.0` - Added new feature
- `v1.1.1` - Bug fix

## Build Environment

### Prerequisites
- Arduino IDE or PlatformIO
- ESP32 Board Support Package
- Adafruit ESP32-S3 Feather board definition

### Board Configuration
```
Board: Adafruit Feather ESP32-S3 4MB Flash 2MB PSRAM
USB Mode: USB-OTG (TinyUSB)
Upload Speed: 921600
```

## I2C Configuration

Default I2C pins for Adafruit ESP32-S3 Feather:
- **SDA:** GPIO 3
- **SCL:** GPIO 4

STEMMA QT connector provides dedicated I2C connection.

## Code Style Guidelines

- Use descriptive variable and function names
- Comment complex logic
- Keep functions focused and single-purpose
- Use `#define` for hardware pin assignments
- Group related functionality into separate files

## File Structure

```
Field_Compass/
├── CLAUDE.md           # This file - project guidelines
├── README.md           # Project documentation
├── src/                # Source code
│   ├── main.cpp        # Main application entry
│   ├── config.h        # Configuration and pin definitions
│   └── ...
├── lib/                # Project-specific libraries
├── include/            # Header files
├── test/               # Test files
└── docs/               # Additional documentation
```

## Claude Assistant Responsibilities

### Automatic Actions
1. **Create Issues** when identifying:
   - New enhancement opportunities
   - Bugs or defects
   - Documentation gaps

2. **Maintain Issues** by:
   - Adding comments for each update
   - Updating Status labels as work progresses
   - Documenting Specifications and Acceptance Criteria

3. **Version Control** by:
   - Committing successfully compiled code
   - Creating version tags following semantic versioning
   - Writing clear commit messages referencing Issues

### Before Each Commit
- [ ] Code compiles without errors
- [ ] Related Issue(s) updated with progress
- [ ] Commit message references Issue number(s)
- [ ] Version tag created if milestone reached

## Quick Reference Commands

```bash
# Create a new Issue (Status and Priority are set via GitHub Project fields)
gh issue create --title "Title" --body "Description" --label "type:enhancement"

# Add comment to Issue
gh issue comment <number> --body "Update comment"

# Create version tag
git tag -a v1.0.0 -m "Release version 1.0.0"
git push origin v1.0.0

# List Issues
gh issue list
```

**Note:** Status and Priority are managed through the GitHub Project board fields, not labels.
