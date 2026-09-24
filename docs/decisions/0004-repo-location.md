# ADR-0004: Repository lives in Sorting_Camera; sorting-cam frozen

- **Status:** Accepted (2026-09-23, confirmed by user)
- **Context:** `D:\Desktop\Sorting_Camera` was empty (only Master Prompt); `D:\Desktop\sorting-cam` holds prior prototype with git history.
- **Decision:** New git repository initialized in `Sorting_Camera/` with the structure required by Master Prompt §30. Sibling prototype is read-only reference (never a source of truth; hardware/docs outrank it per §41).
- **Consequences:** Clean history, no legacy baggage; some knowledge (XCLK/pin behavior) manually carried into docs/ADRs instead of git history.
