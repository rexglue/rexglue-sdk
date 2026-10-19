#include <rex/ui/title_app.h>

rex::PPCImage PPCImageConfig{};

class FooApp : public rex::ui::TitleApp {
 protected:
  void OnCreateWindow(rex::ui::TitleUI& ui) override {
    if (!ui.GetUI().AddOverlays(rex::ui::kStandardOverlays)) {
      rex::RequestShutdown(1);
    }
  }
};

REX_TITLE_APP(FooApp)
