#ifndef slic3r_WizardTypes_hpp_
#define slic3r_WizardTypes_hpp_

namespace Slic3r {
namespace GUI {

// How the setup guide (GuideFrame, the web wizard) is started.
namespace Wizard {

// Why the guide is run
enum RunReason {
    RR_DATA_EMPTY,                  // No or empty datadir
    RR_DATA_LEGACY,                 // Pre-updating datadir
    RR_DATA_INCOMPAT,               // Incompatible datadir - Slic3r downgrade situation
    RR_USER,                        // User requested the guide from the menus
};

// What page the guide starts on
enum StartPage {
    SP_WELCOME,
    SP_PRINTERS,
    SP_FILAMENTS,
    SP_MATERIALS,
    SP_CUSTOM,
};

} // namespace Wizard

} // namespace GUI
} // namespace Slic3r

#endif
