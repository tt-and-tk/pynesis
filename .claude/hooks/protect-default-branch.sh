#!/usr/bin/env bash
# git add / git commit をデフォルトブランチ上で実行しようとした場合に拒否する
set -euo pipefail

input="$(cat)"
command="$(echo "$input" | jq -r '.tool_input.command // empty')"

# git add/commit が複合コマンド(&&・;・|で連結)の途中にあっても検知できるよう，先頭一致ではなく単語境界での部分一致で判定する
if ! printf '%s' "$command" | grep -qE '(^|[;&|`$(]|[[:space:]])git[[:space:]]+(add|commit)([[:space:]]|$)'; then
  exit 0
fi

current_branch="$(git branch --show-current 2>/dev/null || true)"
default_branch="$(git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null | sed 's#^origin/##' || true)"
if [ -z "$default_branch" ]; then
  default_branch="$(git remote show origin 2>/dev/null | sed -n 's/^\s*HEAD branch:\s*//p' || true)"
fi

if [ -n "$current_branch" ] && [ -n "$default_branch" ] && [ "$current_branch" = "$default_branch" ]; then
  echo "{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"permissionDecision\":\"deny\",\"permissionDecisionReason\":\"デフォルトブランチ($default_branch)上でのgit add/commitは禁止されています．issue用のブランチを作成してから実行してください．\"}}"
  exit 0
fi

exit 0
