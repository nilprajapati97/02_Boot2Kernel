# Repo Tool Setup

## Overview

Google's **`repo`** tool is a Git wrapper that manages multiple Git repositories as a single project. Qualcomm's Android BSP (Board Support Package) consists of hundreds of Git repositories — `repo` orchestrates them all via a **manifest file**.

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| OS | Ubuntu 16.04+ (or equivalent Linux) |
| Python | Python 3.6+ |
| Git | Git 2.x+ |
| Disk Space | ~100 GB for full AOSP + Qualcomm BSP |
| RAM | 16 GB+ recommended for builds |

---

## Installing the Repo Tool

### Step 1: Install Dependencies

```bash
sudo apt-get update
sudo apt-get install -y git curl python3 python3-pip

# Git configuration (required by repo)
git config --global user.name "Your Name"
git config --global user.email "your.email@example.com"
```

### Step 2: Download and Install Repo

```bash
# Create a bin directory in home (if not exists)
mkdir -p ~/bin

# Download repo launcher script
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/bin/repo
chmod a+x ~/bin/repo

# Add to PATH (add to ~/.bashrc for persistence)
export PATH=~/bin:$PATH
echo 'export PATH=~/bin:$PATH' >> ~/.bashrc
source ~/.bashrc

# Verify installation
repo version
```

Expected output:
```
repo launcher version 2.x
repo version v2.x
```

---

## How Repo Works

### Core Concepts

```
┌─────────────────────────────────────────────────┐
│                  Manifest Repository             │
│                                                   │
│  default.xml ──── Defines all Git repositories    │
│                   their branches, paths, and      │
│                   remote URLs                     │
└────────────────────────┬────────────────────────┘
                         │
                    repo init
                         │
                    repo sync
                         │
         ┌───────────────┼───────────────┐
         ▼               ▼               ▼
    ┌─────────┐    ┌─────────┐    ┌─────────┐
    │ Git Repo│    │ Git Repo│    │ Git Repo│    ... (hundreds)
    │ kernel/ │    │ device/ │    │ vendor/ │
    └─────────┘    └─────────┘    └─────────┘
```

### Manifest File Structure (`default.xml`)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <!-- Remote server definitions -->
  <remote name="aosp"
          fetch="https://android.googlesource.com" />
  <remote name="qcom"
          fetch="https://source.codeaurora.org" />

  <!-- Default settings for all projects -->
  <default revision="android-9.0.0_r1"
           remote="aosp"
           sync-j="4" />

  <!-- Individual Git repositories -->
  <project path="build/make"
           name="platform/build"
           remote="aosp" />

  <project path="kernel/msm-4.4"
           name="kernel/msm-4.4"
           remote="qcom"
           revision="android-9.0" />

  <project path="device/qcom/sdm660"
           name="device/qcom/sdm660"
           remote="qcom" />

  <!-- ... hundreds more projects ... -->
</manifest>
```

### Key Manifest Elements

| Element | Purpose |
|---------|---------|
| `<remote>` | Defines Git server URLs (AOSP, CodeAurora, etc.) |
| `<default>` | Default branch, remote, and sync settings |
| `<project>` | Individual Git repository: `path` = local dir, `name` = repo name |
| `revision` | Branch or tag to check out |
| `<copyfile>` | Copy a file from a project to a different location |
| `<linkfile>` | Create a symlink from a project to a different location |

---

## Essential Repo Commands

### Initialize a Workspace

```bash
# Create workspace directory
mkdir ~/sdm660_android && cd ~/sdm660_android

# Initialize with manifest
repo init -u <MANIFEST_URL> -b <BRANCH> -m <MANIFEST_FILE>

# Example for Qualcomm SDM660 (Android 9)
repo init -u https://source.codeaurora.org/quic/la/platform/manifest \
          -b release \
          -m LA.UM.7.2.r1-xxxxx-sdm660.0.xml \
          --depth=1
```

| Flag | Purpose |
|------|---------|
| `-u` | Manifest repository URL |
| `-b` | Branch name |
| `-m` | Specific manifest XML file |
| `--depth=1` | Shallow clone (saves disk/time) |

### Sync Source Code

```bash
# Full sync (all repositories)
repo sync -j8 -c --no-tags

# Sync specific project only
repo sync kernel/msm-4.4

# Force sync (discard local changes)
repo sync -j8 -c --force-sync
```

| Flag | Purpose |
|------|---------|
| `-j8` | Use 8 parallel download threads |
| `-c` | Only fetch current branch (faster) |
| `--no-tags` | Skip tag fetching (saves time) |
| `--force-sync` | Overwrite local changes |

### Working with Branches

```bash
# Create a topic branch across all repos
repo start my-feature --all

# Create a branch in specific project
repo start my-feature kernel/msm-4.4

# Check status across all repos
repo status

# Show current branches
repo branches
```

### Other Useful Commands

```bash
# Run a Git command across all repos
repo forall -c 'git log --oneline -1'

# Find which project contains a file
repo forall -c 'test -f drivers/iio/imu/bmi160/bmi160_core.c && echo $REPO_PROJECT'

# Diff across all repos
repo diff
```

---

## Repo Internal Structure

After `repo init`, a `.repo/` directory is created:

```
.repo/
├── manifest.xml          # Symlink to selected manifest
├── manifests/            # Cloned manifest repository
│   ├── default.xml
│   └── snippets/
├── manifests.git/        # Bare git repo for manifests
├── repo/                 # Repo tool source code
└── project-objects/      # Git object stores (shared)
```

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `repo init` fails with SSL error | Set `GIT_SSL_NO_VERIFY=1` or install CA certs |
| Sync hangs on specific repo | `repo sync <project-path>` individually |
| "Cannot checkout" errors | `repo sync --force-sync` to reset |
| Disk space issues | Use `--depth=1` and `-c` flags |
| Permission denied on `repo` | `chmod a+x ~/bin/repo` |

---

## Next Steps

- [02_QCOM_BSP_Source_Sync.md](02_QCOM_BSP_Source_Sync.md) — Sync the Qualcomm SDM660 BSP source
- [03_Source_Tree_Layout.md](03_Source_Tree_Layout.md) — Understand the Android source tree
- [04_Kernel_Source_Setup.md](04_Kernel_Source_Setup.md) — Set up and build the kernel
