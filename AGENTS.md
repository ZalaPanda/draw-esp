# AGENTS.md

## Project Guidelines

The `README.md` is the **single source of truth** for this project's goals, architecture, technical decisions, requirements, constraints, and development roadmap.

### Mandatory Rules

1. **Read the README first.** Before making changes, read the relevant sections of `README.md` and make sure you understand the current project direction.

2. **Follow the documented architecture.** All implementation decisions and code changes must be consistent with the requirements, design principles, and constraints described in the README.

3. **Keep documentation in sync.** When implementation details, architecture, requirements, or project direction change, update the README accordingly. Do not allow the documentation to become outdated or contradict the actual implementation.

4. **Update the README when taking a different direction.** If you discover that a documented approach is technically infeasible, unsuitable, or should be replaced:
   - Explain why the existing approach should change.
   - Update the README to document the new direction and the reasoning behind it.
   - Ensure the implementation and documentation remain consistent.
   - Clearly communicate significant architectural changes to the user.

5. **Do not silently change requirements.** If a proposed change affects core functionality, hardware assumptions, API contracts, security, compatibility, or other important requirements, discuss it with the user before proceeding.

6. **Document important decisions.** Record non-obvious architectural and technical decisions, including relevant trade-offs and limitations, in the appropriate README section.

### Priority

When instructions or assumptions conflict, use the following order:

1. Explicit instructions from the user in the current conversation.
2. The current `README.md`.
3. Existing implementation and code comments.
4. Agent assumptions or preferences.

Never change the project's direction merely because another approach seems more convenient. Prefer consistency, maintainability, and incremental development over unnecessary rewrites.