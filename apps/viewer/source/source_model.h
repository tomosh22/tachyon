// Data model of the Source panel (WP-45): everything that does not need ImGui. buildSourceView() runs on a
// query-executor worker and turns one selected function into an immutable SourceView: the located source
// file (read, capped and checked for binary content), the per-line times of linesForFunction, the function's
// disassembly with the per-instruction times of instructionsForFunction, and the mapping of instructions to
// source lines (mixed mode). The panel only draws a SourceView.
//
// Attribution notes (spec 3.7 + WP-34 clarifications). Line times are the rule-1 weights of linesForFunction
// (non-leaf frames are looked up at rva - 1). Instruction times use the frame's EXACT address, which for a
// caller frame is the return address, i.e. the instruction after the call ("after call"). An instruction
// row is listed under the line of its own address, except that a post-call row with samples is listed under
// the call's line (InstructionRow::groupLine), where linesForFunction attributes caller frames, so the
// per-line sums of the rows match the line totals. Residual case: a leaf sample on a post-call instruction.
#pragma once

#include "analysis/query.h"
#include "core/result.h"
#include "symbols/function_key.h"
#include "symbols/source_locator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::viewer::source {

// Largest source file that is shown; a bigger one is treated like a missing file (with that reason).
inline constexpr size_t kMaxSourceFileBytes = size_t{16} << 20;
// Most lines of a shown file (a 16 MiB file made of line breaks would otherwise need hundreds of MiB of row
// tables); a file with more lines is treated like an oversized one.
inline constexpr uint32_t kMaxSourceLines = 2'000'000;
// Largest function body that is disassembled; a bigger function is cut here (and says so).
inline constexpr uint64_t kMaxDisassemblyBytes = uint64_t{1} << 20;
// Bytes disassembled for an unresolved {module, rva} key (no symbols, so no function bounds).
inline constexpr uint64_t kUnresolvedWindowBytes = 256;
// Tab stops of the source text when it is displayed.
inline constexpr uint32_t kTabWidth = 4;

// Weights of one source line or instruction, in trace ticks.
struct LineTime {
  uint64_t ticks = 0;  // running + ready + waiting
  uint64_t running = 0, ready = 0, waiting = 0;
};

// An immutable text file split into lines. Line numbers are 1-based. Terminators ("\n", "\r\n" and a lone
// "\r") are not part of a line; a final line without a terminator counts, an empty file has no lines.
class SourceText {
 public:
  // Thread-safe (no shared state). Splits `bytes` (a leading UTF-8 BOM is skipped). Fails with CorruptData
  // when the text contains a NUL byte (a binary file) and with InvalidArgument above kMaxSourceFileBytes.
  static Result<std::shared_ptr<const SourceText>> parse(std::string bytes);
  // Thread-safe. Reads and parses `path`. NotFound / IoError when it cannot be read, Cancelled when the flag
  // was raised while reading.
  static Result<std::shared_ptr<const SourceText>> load(const std::filesystem::path& path,
                                                        analysis::Cancel cancel = {});

  // All members: thread-safe (immutable).
  uint32_t lineCount() const { return static_cast<uint32_t>(m_lineStart.size()) - 1; }
  // The text of line `number` (1..lineCount) without its terminator; empty for other numbers.
  std::string_view line(uint32_t number) const;
  // Width of the widest line in character cells (tabs expanded to kTabWidth stops).
  uint32_t maxColumns() const { return m_maxColumns; }
  // True when the line holds only printable ASCII (no tab, control or non-ASCII byte): then display column ==
  // byte offset and the text can be drawn as it is, which keeps a window of a huge line O(window).
  bool lineIsSimple(uint32_t number) const;
  size_t byteSize() const { return m_text.size(); }
  // Smallest line number >= `from` whose text contains `needle` (ASCII case-insensitive); 0 when none.
  uint32_t findNext(std::string_view needle, uint32_t from) const;
  // Largest line number <= `from` that contains `needle`; 0 when none.
  uint32_t findPrev(std::string_view needle, uint32_t from) const;

 private:
  SourceText() = default;

  std::string m_text;
  std::vector<uint32_t> m_lineStart;  // lineCount + 1 offsets; line i spans [start[i-1], end of line)
  std::vector<uint8_t> m_simple;      // lineCount flags, see lineIsSimple
  std::vector<uint32_t> m_lineEnd;    // lineCount offsets (terminator excluded)
  uint32_t m_maxColumns = 0;
};

// Thread-safe (pure function). The line with tabs expanded to kTabWidth stops, control characters shown as
// '.' and every non-ASCII code point as one '?' (the UI font is ASCII only), so that one character is one
// cell. Stops after about `maxColumns` cells (the caller shows a window of a very long line).
std::string expandForDisplay(std::string_view line, size_t maxColumns = SIZE_MAX);
// Thread-safe (pure function). Display column (0-based) of byte offset `offset` of `line`.
uint32_t displayColumn(std::string_view line, size_t offset);
// Thread-safe (pure function). Same, continuing from byte `fromOffset` that is at display column `fromColumn`
// (so a caller walking forward through matches stays linear).
uint32_t displayColumnFrom(std::string_view line, size_t fromOffset, uint32_t fromColumn, size_t offset);
// Thread-safe (pure function). Byte offset of the first character whose cell starts at or after `column`
// (line.size() when the line is shorter); `*reached` receives the column of that offset.
size_t offsetOfColumn(std::string_view line, uint32_t column, uint32_t* reached = nullptr);
// Thread-safe (pure function). ASCII case-insensitive substring search; npos when absent. An empty needle
// matches at 0.
size_t findInsensitive(std::string_view haystack, std::string_view needle, size_t from = 0);

// One decoded instruction of the function with its time.
struct InstructionRow {
  uint64_t rva = 0;
  uint8_t length = 1;
  std::array<uint8_t, 15> bytes{};
  std::string text;   // Intel syntax; call/branch targets carry " <symbol>" when known
  uint32_t line = 0;  // source line of the instruction's address (0 = unknown); see the header note
  // The line the row is listed under in mixed mode: `line`, except that a row with samples that follows a
  // call (afterCall && ticks != 0) sits under the call's line, because linesForFunction attributes a caller
  // frame's return address to the call (rva - 1). Residual case: a LEAF sample that executes on a post-call
  // instruction is attributed to that instruction's own line by the query, but it is listed under the
  // call's line here (the row cannot tell the two kinds of samples apart).
  uint32_t groupLine = 0;
  uint64_t ticks = 0;  // running weight of the samples at an address inside this instruction
  bool isCall = false;
  bool afterCall = false;  // the previous instruction is a call: samples here are return addresses
  bool valid = true;       // false: undecodable bytes ("db 0x..")
};

// What the panel shows, decided by the worker.
enum class ViewKind : uint8_t {
  Message,      // nothing to show: `message` says why (scope, pseudo function, unmapped address, ...)
  Source,       // the source file was located: `file` is set; `instructions` may also be set (mixed mode)
  Disassembly,  // no usable source file: `sourceProblem` says why; `instructions` is the view
};

struct SourceView {
  ViewKind kind = ViewKind::Message;
  std::string message;  // Message
  symbols::FunctionKey key;
  std::string functionName;  // display name without the module
  std::string moduleName;
  bool resolved = false;  // the key is a resolved function (symbols available)

  // ---- source ----
  std::string debugPath;      // the file as recorded in the debug info; empty when none
  std::string localPath;      // the located file (UTF-8); empty when not located
  std::string sourceProblem;  // why there is no source (kind == Disassembly): not found / too large / binary
  std::shared_ptr<const SourceText> file;
  // Indexed by line number, size file->lineCount() + 1; entry 0 is unused. Empty without a file.
  std::vector<LineTime> lines;
  LineTime total;  // sum over every LineAgg of the function, including line 0 and lines past EOF
  uint64_t unknownLineTicks = 0;         // ticks on line 0 (no line information)
  uint64_t pastEndTicks = 0;             // ticks on lines beyond the file (the file differs from the build)
  uint32_t hottestLine = 0;              // 0 = none
  uint32_t firstLine = 0, lastLine = 0;  // the function's own lines in the file (0 = unknown)

  // ---- disassembly ----
  bool haveInstructions = false;
  std::string instructionsProblem;           // why there are none (module image missing, ...)
  uint64_t startRva = 0, sizeBytes = 0;      // the disassembled range
  std::vector<InstructionRow> instructions;  // by address
  uint64_t instructionTicks = 0;             // time inside the decoded range
  uint64_t otherTicks = 0;                   // instruction samples outside it (split function, ...)
  uint32_t hottestInstruction = UINT32_MAX;  // index into `instructions`
  // Mixed mode: instruction indices grouped by source line. lineInstrBegin has lineCount + 2 entries;
  // instructions of line L are lineInstrOrder[lineInstrBegin[L] .. lineInstrBegin[L + 1]) in address order.
  std::vector<uint32_t> lineInstrOrder;
  std::vector<uint32_t> lineInstrBegin;
  std::string note;  // e.g. "split function: showing the piece that contains the entry"

  // Thread-safe (immutable). The instructions mapped to `line` (1-based); empty without a mixed mapping.
  std::vector<uint32_t> instructionsOfLine(uint32_t line) const;
  bool hasLine(uint32_t line) const { return line >= 1 && line < lines.size(); }
  size_t instructionCountOfLine(uint32_t line) const {
    return line + 1 < lineInstrBegin.size() ? lineInstrBegin[line + 1] - lineInstrBegin[line] : 0;
  }
};

// What buildSourceView() needs besides the query context.
struct BuildInputs {
  analysis::Filter filter;  // time range and threads; subtreeRoot/function ignored
  symbols::FunctionKey key;
  std::shared_ptr<symbols::SourceLocator> locator;  // required
  // Replaces the module path of the trace when reading code bytes (tests, relocated binaries).
  std::filesystem::path moduleImageOverride;
};

// Thread-safe (const access to the context; run it on a worker). Builds the view of `inputs.key`: queries,
// locates and reads the source, disassembles. Anything that merely cannot be shown (scope key, missing file,
// missing module image) is reported inside the view; only cancellation is an error.
Result<std::shared_ptr<const SourceView>> buildSourceView(const analysis::QueryContext& ctx,
                                                          const BuildInputs& inputs, analysis::Cancel cancel);

// Thread-safe (pure function). The remap rule that makes `chosen` the file for `debugPath`: the longest
// common run of trailing path components (separators interchangeable, ASCII case-insensitive for
// Windows-looking paths) is stripped from both, the remainders become `from` and `to`. A debug path without a
// common directory prefix to strip cannot form a rule (nullopt); the caller then adds the chosen file's
// directory as a search root instead (see locateOutcome()).
std::optional<symbols::SourceRemapRule> deriveRemapRule(std::string_view debugPath,
                                                        const std::filesystem::path& chosen);

// How a file chosen with "Locate file..." is made known to the locator.
struct LocateOutcome {
  std::optional<symbols::SourceRemapRule> rule;  // preferred: a prefix rule
  std::optional<std::filesystem::path> root;     // otherwise a search root
};
// Thread-safe (pure function). `chosen` may be a file (rule, or its directory as a root when no rule can be
// derived) or a directory (a root).
LocateOutcome locateOutcome(std::string_view debugPath, const std::filesystem::path& chosen,
                            bool chosenIsDirectory);

// Thread-safe (pure function). The log-scaled heat of `ticks` against `maxTicks`, 0..1 (0 for no time).
float heatOf(uint64_t ticks, uint64_t maxTicks);

}  // namespace tachyon::viewer::source
