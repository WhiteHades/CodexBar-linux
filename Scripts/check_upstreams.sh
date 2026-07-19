#!/usr/bin/env bash
# Check for new changes in upstream repositories
# Usage: ./Scripts/check_upstreams.sh [upstream|quotio|all]

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

TARGET=${1:-all}
DAYS=${2:-7}

case "$TARGET" in
  all|upstream|quotio)
    ;;
  *)
    echo -e "${RED}Error: Unknown target '$TARGET'. Use all, upstream, or quotio.${NC}" >&2
    echo -e "${RED}Usage: ./Scripts/check_upstreams.sh [upstream|quotio|all]${NC}" >&2
    exit 1
    ;;
esac

if { [ "$TARGET" = "all" ] || [ "$TARGET" = "quotio" ]; } && [[ ! "$DAYS" =~ ^[0-9]+$ ]]; then
  echo -e "${RED}Error: Days must be a non-negative integer.${NC}" >&2
  exit 1
fi

ensure_remote() {
    local name=$1
    local url=$2
    if ! git remote get-url "$name" >/dev/null 2>&1; then
        echo -e "${YELLOW}Adding $name remote...${NC}"
        git remote add "$name" "$url"
    fi
    git fetch "$name"
}

echo -e "${BLUE}==> Fetching upstream changes...${NC}"
if [ "$TARGET" = "all" ] || [ "$TARGET" = "upstream" ]; then
    ensure_remote upstream https://github.com/steipete/CodexBar.git
fi

if [ "$TARGET" = "all" ] || [ "$TARGET" = "quotio" ]; then
    ensure_remote quotio https://github.com/nguyenphutrong/quotio.git
fi

echo ""

remote_default_branch() {
    local remote=$1
    local branch=""
    local candidate

    branch=$(git symbolic-ref -q --short "refs/remotes/${remote}/HEAD" 2>/dev/null | sed "s#^${remote}/##" || true)
    if [ -z "$branch" ]; then
        branch=$(git remote show "$remote" 2>/dev/null | awk '/HEAD branch/ {print $NF; exit}' || true)
    fi
    if [ -n "$branch" ] && git rev-parse --verify -q "${remote}/${branch}" >/dev/null; then
        echo "$branch"
        return 0
    fi

    for candidate in main master; do
        if git rev-parse --verify -q "${remote}/${candidate}" >/dev/null; then
            echo "$candidate"
            return 0
        fi
    done

    echo -e "${RED}Error: Could not resolve default branch for remote '$remote'.${NC}" >&2
    exit 1
}

local_default_branch() {
    local branch=""
    branch=$(git symbolic-ref -q --short refs/remotes/origin/HEAD 2>/dev/null | sed 's#^origin/##' || true)
    if [ -z "$branch" ] || ! git rev-parse --verify -q "origin/${branch}" >/dev/null; then
        for branch in main master; do
            if git rev-parse --verify -q "origin/${branch}" >/dev/null; then
                break
            fi
            branch=""
        done
    fi
    if [ -z "$branch" ] || ! git rev-parse --verify -q "origin/${branch}" >/dev/null; then
        echo -e "${RED}Error: Could not resolve local default branch for diff base.${NC}" >&2
        exit 1
    fi
    echo "$branch"
}

# Check upstream (steipete)
if [ "$TARGET" = "all" ] || [ "$TARGET" = "upstream" ]; then
    echo -e "${BLUE}==> Upstream (steipete/CodexBar) changes:${NC}"
    BASE_BRANCH=$(local_default_branch)
    UPSTREAM_BRANCH=$(remote_default_branch upstream)
    UPSTREAM_REF="upstream/${UPSTREAM_BRANCH}"
    
    UPSTREAM_COUNT=$(git log --oneline "origin/${BASE_BRANCH}..${UPSTREAM_REF}" --no-merges 2>/dev/null | wc -l | tr -d ' ')
    
    if [ "$UPSTREAM_COUNT" -gt 0 ]; then
        echo -e "${GREEN}Found $UPSTREAM_COUNT new commits${NC}"
        echo ""
        git log -20 --oneline --graph "origin/${BASE_BRANCH}..${UPSTREAM_REF}" --no-merges
        echo ""
        echo -e "${YELLOW}Files changed:${NC}"
        git diff --stat "origin/${BASE_BRANCH}..${UPSTREAM_REF}" | tail -20 || true
    else
        echo -e "${GREEN}No new commits (up to date)${NC}"
    fi
    echo ""
fi

# Check quotio
if [ "$TARGET" = "all" ] || [ "$TARGET" = "quotio" ]; then
    echo -e "${BLUE}==> Quotio changes (last $DAYS days):${NC}"
    QUOTIO_BRANCH=$(remote_default_branch quotio)
    QUOTIO_REF="quotio/${QUOTIO_BRANCH}"
    
    QUOTIO_COUNT=$(git log --oneline "$QUOTIO_REF" --since="$DAYS days ago" 2>/dev/null | wc -l | tr -d ' ')
    
    if [ "$QUOTIO_COUNT" -gt 0 ]; then
        echo -e "${GREEN}Found $QUOTIO_COUNT commits in last $DAYS days${NC}"
        echo ""
        git log -20 --oneline --graph "$QUOTIO_REF" --since="$DAYS days ago"
        echo ""
        echo -e "${YELLOW}Recent file changes:${NC}"
        # Show changes from last 10 commits
        git diff --stat "${QUOTIO_REF}~10..${QUOTIO_REF}" 2>/dev/null | tail -20 || echo "Unable to show diff"
    else
        echo -e "${GREEN}No new commits in last $DAYS days${NC}"
    fi
    echo ""
fi

# Summary
echo -e "${BLUE}==> Summary${NC}"
if [ "$TARGET" = "all" ] || [ "$TARGET" = "upstream" ]; then
    echo "Upstream commits: $UPSTREAM_COUNT"
fi
if [ "$TARGET" = "all" ] || [ "$TARGET" = "quotio" ]; then
    echo "Quotio commits (${DAYS}d): $QUOTIO_COUNT"
fi

echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  Review upstream: ./Scripts/review_upstream.sh upstream"
echo "  Review quotio:   ./Scripts/review_upstream.sh quotio"
if [ "$TARGET" = "all" ] || [ "$TARGET" = "upstream" ]; then
    echo "  Detailed diff:   git diff origin/$(local_default_branch)..${UPSTREAM_REF}"
fi
echo "  View quotio:     ./Scripts/analyze_quotio.sh"
