// WP-48: the keyboard shortcut cheat sheet. `?` (Shift+/) opens it, it lists the registered commands with
// their shortcuts and the panels' own keys grouped by panel, and it does not open while a text input has the
// keyboard.
#include "viewer/status/shortcut_sheet.h"

#include "status_test_util.h"

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

namespace {

const ShortcutEntry* find(const std::vector<ShortcutGroup>& groups, const std::string& group,
                          const std::string& description) {
  for (const ShortcutGroup& g : groups) {
    if (g.name != group) continue;
    for (const ShortcutEntry& e : g.entries) {
      if (e.description == description) return &e;
    }
  }
  return nullptr;
}

void pressQuestionMark(Harness& h) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiKey_LeftShift, true);
  io.AddKeyEvent(ImGuiKey_Slash, true);
  io.AddInputCharacter('?');
  h.frames(1);
  io.AddKeyEvent(ImGuiKey_Slash, false);
  io.AddKeyEvent(ImGuiKey_LeftShift, false);
  h.frames(2);
}

}  // namespace

TEST(StatusShortcuts, SheetIsGroupedFromCommandsAndPanelShortcuts) {
  CommandRegistry commands;
  commands.add({"file.open", "File: Open Trace...", "Ctrl+O", nullptr});
  commands.add({"stats.search", "Function Statistics: Search Functions", "Ctrl+F", nullptr});
  commands.add({"misc", "Do Something", "", nullptr});
  ShortcutRegistry shortcuts;
  shortcuts.add("Timeline", "Home", "Fit the whole trace");
  shortcuts.add("Function Statistics", "F3", "Next instance");
  shortcuts.add("Timeline", "Home", "Fit everything");  // same group and keys: replaced
  const std::vector<ShortcutGroup> groups = buildShortcutSheet(commands, shortcuts);
  ASSERT_EQ(groups.size(), 4u);
  EXPECT_EQ(groups[0].name, "File");
  EXPECT_EQ(groups[1].name, "Function Statistics");
  EXPECT_EQ(groups[2].name, "General");
  EXPECT_EQ(groups[3].name, "Timeline");
  ASSERT_EQ(groups[1].entries.size(), 2u);  // the command and the panel key share a group
  EXPECT_EQ(groups[1].entries[0].keys, "Ctrl+F");
  EXPECT_EQ(groups[1].entries[1].keys, "F3");
  ASSERT_EQ(groups[3].entries.size(), 1u);
  EXPECT_EQ(groups[3].entries[0].description, "Fit everything");
  const ShortcutEntry* open = find(groups, "File", "Open Trace...");
  ASSERT_NE(open, nullptr);
  EXPECT_EQ(open->keys, "Ctrl+O");
  EXPECT_EQ(find(groups, "General", "Do Something")->keys, "");
}

TEST(StatusShortcuts, QuestionMarkOpensTheSheetWithAllCommands) {
  Harness h;
  h.frames(3);
  EXPECT_FALSE(h.app.shortcutSheet().isOpen());
  pressQuestionMark(h);
  ASSERT_TRUE(h.app.shortcutSheet().isOpen());
  h.frames(2);

  const std::vector<ShortcutGroup>& groups = h.app.shortcutSheet().lastGroups();
  ASSERT_FALSE(groups.empty());
  // Every registered command is listed with its shortcut.
  for (const Command& c : h.app.commands().commands()) {
    std::string title = c.title;
    if (const size_t cut = title.find(": "); cut != std::string::npos) title = title.substr(cut + 2);
    bool found = false;
    for (const ShortcutGroup& g : groups) {
      for (const ShortcutEntry& e : g.entries)
        found = found || (e.description == title && e.keys == c.shortcut);
    }
    EXPECT_TRUE(found) << c.id;
  }
  // Panel keys, grouped by panel.
  const ShortcutEntry* pan = find(groups, "Timeline", "Pan left / right (Shift: faster)");
  ASSERT_NE(pan, nullptr);
  EXPECT_EQ(pan->keys, "A / Left, D / Right");
  EXPECT_NE(find(groups, "Function Statistics", "Next / previous instance in the timeline"), nullptr);
  EXPECT_NE(find(groups, "Function Statistics", "Search Functions"), nullptr);
  const ShortcutEntry* sheet = find(groups, "Help", "Keyboard Shortcuts");
  ASSERT_NE(sheet, nullptr);
  EXPECT_EQ(sheet->keys, "?");

  // Escape closes it.
  h.key(ImGuiKey_Escape);
  h.frames(2);
  EXPECT_FALSE(h.app.shortcutSheet().isOpen());
}

TEST(StatusShortcuts, PanelsCanRegisterTheirOwnKeys) {
  Harness h;
  h.app.shortcuts().add("My Panel", "Ctrl+K", "Do the thing");
  h.app.commands().run("help.shortcuts");
  h.frames(3);
  ASSERT_TRUE(h.app.shortcutSheet().isOpen());
  EXPECT_NE(find(h.app.shortcutSheet().lastGroups(), "My Panel", "Do the thing"), nullptr);
}

TEST(StatusShortcuts, DoesNotOpenWhileTyping) {
  Harness h;
  h.frames(3);
  // Open the command palette: its query field has the keyboard.
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_P, true);
  h.frames(1);
  io.AddKeyEvent(ImGuiKey_P, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  h.frames(4);
  ASSERT_TRUE(io.WantTextInput);
  pressQuestionMark(h);
  EXPECT_FALSE(h.app.shortcutSheet().isOpen());
  // Close the palette; now `?` works.
  h.key(ImGuiKey_Escape);
  h.frames(3);
  ASSERT_FALSE(io.WantTextInput);
  pressQuestionMark(h);
  EXPECT_TRUE(h.app.shortcutSheet().isOpen());
}

TEST(StatusShortcuts, HelpMenuCommandAndPaletteEntry) {
  Harness h;
  bool listed = false;
  for (const Command& c : h.app.commands().commands())
    listed = listed || (c.id == "help.shortcuts" && c.shortcut == "?");
  EXPECT_TRUE(listed);
  h.app.commands().run("help.shortcuts");
  h.frames(2);
  EXPECT_TRUE(h.app.shortcutSheet().isOpen());
}
