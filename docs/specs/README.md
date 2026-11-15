# Shared specifications (orchestrator-owned)

These files are the living, normative specifications. They started as copies of `docs/IMPLEMENTATION_PLAN.md` Part 3 and have since been updated with every work package; where the plan's Part 3 differs, **these files and the matching headers win**. Work packages conform to these; proposed changes go in the WP report and are merged only by the orchestrator, who updates this directory and the matching header together. Dated "Clarifications" sections are binding and record when and why a rule was added. Last full review against the code: 2026-11-15 (origin/main at WP-67f).

| Part | File | Header / source of truth |
|---|---|---|
| 3.1 | trace-format.md | libs/trace/trace_format.h, trace_writer.h, trace_reader.h |
| 3.2 | model.md | libs/model/model.h |
| 3.3 | sdk-api.md | sdk/include/tachyon/tachyon.h, tachyon_loader.h |
| 3.4 | control-protocol.md | libs/control/protocol.h |
| 3.5 | capture-backend.md | libs/capture/capture_backend.h |
| 3.6 | symbol-provider.md | libs/symbols/symbol_provider.h, function_key.h, symbol_service.h, symbol_locator.h |
| 3.7 | analysis-query.md | libs/analysis/query.h (plus instances.h, scope_pairing.h, fibers.h) |
| 3.8 | attribution-rules.md | normative rules; libs/analysis/thread_states.h, wait_reasons.h, scope_blend.h |
| 3.9 | timeline-lod.md | libs/analysis/lod.h |
| WP-51 | inst-wire.md | sdk/src/inst_wire.h |
| WP-65 | live.md | libs/model/live_model.h (batch encoding), libs/control/live_encoder.h, libs/capture/capture_backend.h (`ILiveSink`) |

## Fixture schemas (orchestrator-owned since the WP-04 merge, 2026-10-25)
`tools/tracegen/README.md` defines the scenario schema plus `tachyon-symbols-1` (`<name>.symbols.json`, the contract for the `JsonSymbolProvider`) and `tachyon-expected-1` (`<name>.expected.json`, the ground truth for the analysis tests and the WP-53 blending and fiber checks). Changes to those two schemas go through the orchestrator. Fixtures are generated at build time into `build/<preset>/fixtures/` (target `tachyon_fixtures`; `tachyon_fixtures_large` is opt-in) and checked byte-for-byte against `tests/fixtures/tracegen/manifest.json`. Tests reach them through the `TACHYON_GENERATED_FIXTURE_DIR` compile definition.

## Instrumentation wire format (orchestrator-owned since the WP-51 merge, 2026-10-25)
`sdk/src/inst_wire.h` is the normative definition and `docs/specs/inst-wire.md` is its canonical prose description (`sdk/src/inst_wire.md` is only a pointer to it since 2026-11-15). They define the SDK → agent stream and the identical `.tachyon-inst` sidecar format. The agent's ingestion (`libs/inst`, WP-52) and later tools must follow it. `Dropped` frames are converted to `Marker("tachyon.dropped", data = count)`.

## Changelog of this directory
- 2026-11-15: full review against origin/main (WP-67f). Clarification sections folded into the main text where that made the spec clearer (dated lines kept); headers, defaults and JSON keys brought up to date (live view, remote capture, flight recorder, CaptureInfo keys, `CaptureOptions::live` and `reservedOutput`, `CaptureResult::warnings`, Linux wait reason 0 = unknown); `inst-wire.md` made canonical.
