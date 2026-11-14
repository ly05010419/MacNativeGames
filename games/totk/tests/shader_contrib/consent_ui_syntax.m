// Compile the real contribution page without loading game code or opening a window.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#define TOTK_GAME_VERSION "131072"
#include "totk_contrib_upload.h"
static NSString *settings_text(const char *zh, const char *en) { return @(TOTK_UI(zh, en)); }
enum { SETTINGS_PAGE_CONTRIB = 0 };
@interface TOTKSettingsPanel : NSView
@property(strong) NSArray<NSView *> *pages;
@end
#include "contrib_tab.h"
