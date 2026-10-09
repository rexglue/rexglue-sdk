#include <rex/ui/title_app.h>

rex::PPCImageInfo PPCImageConfig{};

class FooApp : public rex::ui::TitleApp {
 protected:
  void OnCreateWindow(rex::ui::TitleUI& ui) override {
    if (!ui.Overlays().AddOverlays(rex::ui::kStandardOverlays)) {
      rex::RequestShutdown(1);
    }
  }
};

REX_TITLE_APP(FooApp)
