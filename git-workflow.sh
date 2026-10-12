#!/bin/bash
# git-workflow.sh  --  drives the exact branch / commit / tag / release
#                     sequence the assignment requires.
#
# USAGE
#   ./git-workflow.sh setup      Feature-1: initial commit on main
#   ./git-workflow.sh all        every feature in order
#
# IMPORTANT
#   Run it from INSIDE your cloned ROLL_NO-OS-A02 directory, and run each
#   stage from the correct starting point. The script refuses to re-run a
#   stage whose branch already exists, so it is safe to re-invoke after a
#   mistake.

set -euo pipefail

cd "$(dirname "$0")"

MSG() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ERR() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- helpers --

build_and_verify() {
    MSG "Building and verifying src/ls.c"
    make clean >/dev/null
    make
    [ -x bin/ls ] || ERR "bin/ls was not produced"
    echo "--- ./bin/ls            ---"; ./bin/ls            || true
    echo "--- ./bin/ls -l         ---"; ./bin/ls -l         || true
    echo "--- ./bin/ls -x         ---"; ./bin/ls -x         || true
    echo "--- ./bin/ls -R src     ---"; ./bin/ls -R src     || true
}

branch_exists() { git rev-parse --verify "$1" >/dev/null 2>&1; }
tag_exists()    { git rev-parse --verify "$1" >/dev/null 2>&1; }

# Merge the current feature branch into main, then tag and push.
finish_feature() {
    local branch="$1" tag="$2" title="$3"

    MSG "Merging $branch into main"
    git checkout main
    git merge --no-ff "$branch" -m "Merge $branch"

    if tag_exists "$tag"; then
        MSG "Tag $tag already exists - skipping tag creation"
    else
        MSG "Creating annotated tag $tag"
        # -a = annotated, -m = message.  An annotated tag (not a lightweight
        # one) is what makes the GitHub Releases page show a description.
        git tag -a "$tag" -m "$title"
    fi

    MSG "Pushing main and $tag"
    git push origin main
    git push origin "$tag"

    MSG "GitHub release to create manually at:"
    echo "   https://github.com/<you>/<repo>/releases/new?tag_name=$tag"
    echo "   Title : $title"
    echo "   Asset : bin/ls   (upload the compiled binary)"
}

# ------------------------------------------------------------------ stages --

stage_setup() {
    MSG "FEATURE 1: Initial project setup"
    branch_exists main || git checkout -b main

    git add src Makefile versions bin obj man REPORT.md README.md git-workflow.sh
    git status --short
    git commit -m "feat: Initial project setup with starter code"
    git push -u origin main

    cat <<'EOF'

>>> NEXT: create the GitHub repository named ROLL_NO-OS-A02 (uppercase),
    initialised with a README.md, then push it to that remote:

        git remote add origin https://github.com/<user>/ROLL_NO-OS-A02.git
        git push -u origin main

EOF
}

# Each feature stage: create branch -> copy source in -> build -> commit
# -> merge -> tag -> push.
feature_stage() {
    local ver="$1" branch="$2" msg="$3" title="$4"

    MSG "FEATURE: $ver  ($branch)"
    branch_exists "$branch" || git checkout -b "$branch"
    git checkout "$branch"

    # The project evolves one file: src/ls.c. Restoring the snapshot for
    # this version is exactly "what src/ls.c looked like when we tagged".
    cp "versions/$ver.c" src/ls.c

    build_and_verify

    git add -A src Makefile REPORT.md
    git status --short
    git commit -m "$msg"

    finish_feature "$branch" "$ver" "$title"
}

stage_all() {
    stage_setup
    feature_stage "v1.1.0" "feature-long-listing-v1.1.0" \
        "feat: implement -l long listing format (stat, getpwuid, getgrgid, ctime)" \
        "Version 1.1.0: Complete Long Listing Format"
    feature_stage "v1.2.0" "feature-column-display-v1.2.0" \
        "feat: column display down-then-across with ioctl terminal width" \
        "Version 1.2.0: Column Display"
    feature_stage "v1.3.0" "feature-horizontal-display-v1.3.0" \
        "feat: add -x horizontal column display and display-mode enum" \
        "Version 1.3.0: Horizontal Column Display (-x)"
    feature_stage "v1.4.0" "feature-alphabetical-sort-v1.4.0" \
        "feat: sort directory entries alphabetically with qsort" \
        "Version 1.4.0: Alphabetical Sort"
    feature_stage "v1.5.0" "feature-colorized-output-v1.5.0" \
        "feat: colour output by file type using ANSI escape codes" \
        "Version 1.5.0: Colorized Output"
    feature_stage "v1.6.0" "feature-recursive-listing-v1.6.0" \
        "feat: add -R recursive listing" \
        "Version 1.6.0: Recursive Listing (-R)"

    MSG "FEATURE 1 + all features done. Now push every feature branch:"
    cat <<'EOF'

        git push origin --all

    Then push every tag as well (harmless if already pushed):

        git push origin --tags

EOF
}

# -------------------------------------------------------------------- main --

case "${1:-}" in
    setup) stage_setup ;;
    all)   stage_all ;;
    *) cat <<EOF
usage: $0 <setup|all>

  setup   Feature-1 only: commit the project skeleton on main and push.
  all     Run every feature stage in order (branches, commits, merges, tags).

EOF
    exit 1 ;;
esac