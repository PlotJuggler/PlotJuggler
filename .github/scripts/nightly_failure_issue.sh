#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Keeps one open issue per red nightly workflow, so a failure reaches the maintainer by GitHub's issue email:
# a failed or timed-out run opens "Nightly failure: <workflow>" (assigned, label nightly-failure) or comments on the
# open one; a run whose jobs did the work and all passed closes it. A run the main-changed gate skipped (main did
# not move) proves nothing and leaves the issue as it is.
#
# Usage: nightly_failure_issue.sh <run-id> [--apply]   (without --apply it prints what it would do)
# Needs: gh (GH_TOKEN, GH_REPO), jq. NIGHTLY_ASSIGNEE: the GitHub login to assign.
set -euo pipefail

run_id="${1:?usage: nightly_failure_issue.sh <run-id> [--apply]}"
apply=false
[[ "${2:-}" == "--apply" ]] && apply=true
assignee="${NIGHTLY_ASSIGNEE:?set NIGHTLY_ASSIGNEE}"
label="nightly-failure"

run_json="$(gh run view "$run_id" --json workflowName,conclusion,headSha,url,createdAt,jobs)"
workflow="$(jq -r .workflowName <<<"$run_json")"
conclusion="$(jq -r .conclusion <<<"$run_json")"
title="Nightly failure: ${workflow}"
open_issue="$(gh issue list --state open --label "$label" --limit 100 --json number,title |
  jq -r --arg t "$title" '.[] | select(.title == $t) | .number' | head -n1)"

act() {
  if $apply; then
    "$@"
  else
    printf 'would run:'
    printf ' %q' "$@"
    printf '\n'
  fi
}

case "$conclusion" in
  failure | timed_out)
    failed_jobs="$(jq -r '.jobs[] | select(.conclusion == "failure" or .conclusion == "timed_out")
      | "- [\(.name)](\(.url))"' <<<"$run_json")"
    body="$(printf '%s on `%s` (%s): **%s**\n\nFailed jobs:\n%s\n\nRun: %s\n' \
      "$workflow" "$(jq -r '.headSha[0:9]' <<<"$run_json")" "$(jq -r '.createdAt[0:10]' <<<"$run_json")" \
      "$conclusion" "${failed_jobs:-"(none reported)"}" "$(jq -r .url <<<"$run_json")")"
    if [[ -n "$open_issue" ]]; then
      act gh issue comment "$open_issue" --body "$body"
    else
      act gh label create "$label" --force --color B60205 --description "A scheduled CI run failed"
      act gh issue create --title "$title" --label "$label" --assignee "$assignee" --body "$body"
    fi
    ;;
  success)
    # The gate jobs succeed even when the work was skipped.
    ran="$(jq '[.jobs[] | select(.name != "changed" and .name != "gate" and .conclusion == "success")] | length' \
      <<<"$run_json")"
    if [[ -n "$open_issue" && "$ran" -gt 0 ]]; then
      act gh issue close "$open_issue" --comment "Green again: $(jq -r .url <<<"$run_json")"
    else
      echo "${workflow}: success, open issue '${open_issue:-none}', jobs that ran: ${ran}; nothing to do"
    fi
    ;;
  *)
    echo "${workflow}: ${conclusion}; nothing to do"
    ;;
esac
