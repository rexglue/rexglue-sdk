#include <rex/app/title_app.h>

const rex::ppc::PPCImage kPPCImageConfig{};

class FooApp : public rex::app::TitleApp {
 public:
  FooApp() : TitleApp("foo", kPPCImageConfig) {}
};

REX_TITLE_APP(FooApp)
