# Issue tracking

Keep user-observed issues in [CURRENT_ISSUES.md](CURRENT_ISSUES.md) as a simple list without dates or tracking fields.

After fixing an issue and verifying that the reported behavior is resolved, remove its item from the list. Do not remove an item merely because a potential fix was implemented or a build passed. If runtime verification requires the user, keep the item until they confirm the fix.

# Verification policy

Never run manual QA-like game sessions (driving the game through menus, smoke servers, browser automation of gameplay) to verify or diagnose a fix — the project is too complex for that to be worth it. Diagnosis works like this instead:

- Mine `.wgpuc` frame captures, the existing runtime logs, and the lifted code statically.
- When static analysis is exhausted, add small, bounded, env-gated diagnostic logs to the runtime and ask the user to do one run and share the output.
- Subagents implement code (and may write standalone unit tests); they never investigate, build the game, or run it.
