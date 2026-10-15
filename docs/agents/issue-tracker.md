# Learning issue tracker: GitHub

Repository: `immorcoding/notes`. Learning specs, substantial tasks, and questions worth tracking live in GitHub Issues. Casual questions do not require issues. Durable notes live in the repository; experiments may live in a separate Lab repository and should be linked as evidence.

Use `gh` from `/workspace/notes`, or explicitly pass `--repo immorcoding/notes`. Check access with `gh api repos/immorcoding/notes`. Git clone/push access does not establish Issues API access. Do not print credentials.

- Read: `gh issue view NUMBER --repo immorcoding/notes --json number,title,body,labels,comments`.
- List: `gh issue list --repo immorcoding/notes --state open`.
- Create: `gh issue create --repo immorcoding/notes --title TITLE --body-file BODY_FILE`.
- Update labels with `gh issue edit`; close with `gh issue close`. Use a body file for multiline comments.
- Follow session authorization before publishing issues, comments, or other external messages. Discussion alone is not permission to create tickets.

## Learning specs

Keep specs lightweight. Record:

1. The core question and why it matters.
2. Scope and explicit stopping boundaries.
3. Expected artifacts: notes, execution-path analysis, comparisons, or experiments.
4. Evidence: what the learner should explain or predict independently; what observations support the conclusion; remaining uncertainty. A passing agent-built experiment alone does not prove understanding.
5. A review date: decide whether to finish, narrow, continue, pause, or stop; record the reason.

New discoveries can enter the backlog without expanding the current spec. Exploration and engineering work may use different evidence. Do not impose fixed durations, a maximum active-spec count, or a candidate experiment list without agreement.

## Dependencies

Represent ticket prerequisites using native GitHub issue dependencies where available. Resolve the blocker's database `id` with `gh api repos/immorcoding/notes/issues/NUMBER --jq .id`, then add it to the dependent issue using `gh api --method POST repos/immorcoding/notes/issues/DEPENDENT/dependencies/blocked_by -F issue_id=BLOCKER_DATABASE_ID`. This needs API permissions and may be unavailable; report failures.

If native dependencies are unavailable, record `Blocked by: #NUMBER` in the dependent issue and link across repositories explicitly. Do not claim this is a native dependency. A sub-issue relationship groups work; it does not itself establish blocking.

Dependencies do not use the `waiting` label. Labels are defined in `triage-labels.md`.

## Scope

PRs as a request surface: no.

Matt's `/triage` engineering state machine is not configured. Other skills may assist with teaching, research, questions, specs, and tasks when invoked or applicable; adapt deliverables to the agreed learning objective, without modifying installed skills. Creating a spec does not establish that it is approved for execution.
