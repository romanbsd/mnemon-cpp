#!/usr/bin/env bash
# Build a synthetic upstream-mnemon repo at $1 with 6 commits covering each
# classifier class. Idempotent: nukes $1 first.
set -euo pipefail
DEST="${1:-}"
[[ -n "$DEST" ]] || { echo "usage: $0 <dest>" >&2; exit 1; }

rm -rf "$DEST"
git init -q -b main "$DEST"
cd "$DEST"
git config user.email "fixture@test"
git config user.name "Fixture"
git config commit.gpgsign false

# Commit 0 (baseline): empty seed
mkdir -p .keep
touch .keep/.gitkeep
git add .keep
git commit -q -m "Initial seed"

# Commit 1: doc-meta-only (README addition)
echo "# upstream" > README.md
git add README.md
git commit -q -m "Add README"

# Commit 2: asset-only
mkdir -p internal/setup/assets/skills
echo "skill content v1" > internal/setup/assets/skills/foo.md
git add internal/setup/assets
git commit -q -m "Add foo skill asset"

# Commit 3: relevant (Go internal change)
mkdir -p internal/auth
cat > internal/auth/session.go <<'EOF'
package auth

func NewSession() string { return "v1" }
EOF
git add internal/auth
git commit -q -m "Add session helper"

# Commit 4: mixed (asset + Go) - classifier should report 'relevant' since
# 'has_other' wins over 'has_asset'.
echo "skill content v2" > internal/setup/assets/skills/foo.md
mkdir -p internal/cmd
cat > internal/cmd/main.go <<'EOF'
package cmd

func Run() {}
EOF
git add internal/setup/assets internal/cmd
git commit -q -m "Update foo and add cmd.Run"

# Commit 5: relevant + tag v0.0.1
cat > internal/auth/session.go <<'EOF'
package auth

func NewSession() string { return "v2" }
EOF
git add internal/auth
git commit -q -m "Bump session to v2"
git tag v0.0.1
