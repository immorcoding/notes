# Learning workflow labels

These are learning workflow states, not a mapping to Matt's five engineering triage roles. Do not invoke `/triage` assuming such a mapping.

| Label | Meaning |
| --- | --- |
| `backlog` | A worthwhile discovery or question saved for later; no current commitment. |
| `active` | A learning objective currently being pursued. |
| `waiting` | Paused by an external prerequisite, such as hardware, tooling, information, or a human decision. Record the reason, release condition, and next check. |

Use one of these state labels at a time on tracked learning work. Ticket-to-ticket dependencies are recorded separately and do not automatically cause `waiting`. An active spec can contain dependent tasks.

Reading, discussion, experimentation, and writing belong in task checklists rather than state labels; these activities may repeat.

Close completed issues with conclusions and evidence links. Close stopped or abandoned work with the reason, without claiming it was completed. Paused work can remain open with its state and resumption condition recorded. Closed issues are no longer in progress; remove the workflow state label when closing.
