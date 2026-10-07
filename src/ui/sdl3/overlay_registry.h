#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay.h>

namespace rex::ui {

class BindTable;
class ImGuiDrawer;
struct ModuleOverlayContext;

class OverlayRegistry : public NonCopyable {
 public:
  OverlayRegistry(ImGuiDrawer& drawer, BindTable& binds, const ModuleOverlayContext& context);
  ~OverlayRegistry();

  bool AnyVisible() const;
  [[nodiscard]] Result<void> Add(std::span<const OverlayDesc> table);
  void Show(std::string_view name, bool visible);
  void Toggle(std::string_view name);
  ImGuiDialog* Get(std::string_view name);
  void Clear();

 private:
  struct Entry {
    std::string name;
    std::unique_ptr<ImGuiDialog> overlay;
    bool bound = false;
  };

  Entry* Find(std::string_view name);

  ImGuiDrawer& drawer_;
  BindTable& binds_;
  const ModuleOverlayContext& context_;
  std::vector<Entry> entries_;
};

}  // namespace rex::ui
