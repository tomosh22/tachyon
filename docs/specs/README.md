# Shared specifications (orchestrator-owned)

Verbatim copies of `docs/IMPLEMENTATION_PLAN.md` Part 3. Work packages conform to these; proposed changes go in the WP report and are merged only by the orchestrator, who updates the plan, this directory, and the matching header together.

| Part | File | Header |
|---|---|---|
| 3.1 | trace-format.md | libs/trace/trace_format.h, trace_writer.h, trace_reader.h |
| 3.2 | model.md | libs/model/model.h |
| 3.3 | sdk-api.md | sdk/include/tachyon/tachyon.h |
| 3.4 | control-protocol.md | libs/control/protocol.h |
| 3.5 | capture-backend.md | libs/capture/capture_backend.h |
| 3.6 | symbol-provider.md | libs/symbols/symbol_provider.h |
| 3.7 | analysis-query.md | libs/analysis/query.h |
| 3.8 | attribution-rules.md | (normative rules, no header) |
| 3.9 | timeline-lod.md | libs/analysis/lod.h |

## Fixture schemas (orchestrator-owned since the WP-04 merge, 2026-10-25)
`tools/tracegen/README.md` defines the scenario schema plus `tachyon-symbols-1` (`<name>.symbols.json`, the contract for WP-20's JsonSymbolProvider) and `tachyon-expected-1` (`<name>.expected.json`, the ground truth for WP-30–36 and WP-53). Changes to those two schemas go through the orchestrator. Fixtures are generated at build time into `build/<preset>/fixtures/` (target `tachyon_fixtures`; `tachyon_fixtures_large` is opt-in) and checked byte-for-byte against `tests/fixtures/tracegen/manifest.json`. Tests reach them through the `TACHYON_GENERATED_FIXTURE_DIR` compile definition.
