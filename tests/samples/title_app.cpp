#include <rex/app/title_app.h>

rex::ppc::PPCImage PPCImageConfig{};

class FooApp : public rex::app::TitleApp {
 public:
  FooApp() : TitleApp("foo", PPCImageConfig) {}
};

REX_TITLE_APP(FooApp)
