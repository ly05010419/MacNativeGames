// Touch gestures as mouse actions (runtime/hle/touch_gestures.h): each
// gesture of the table in the header, with the actions written out by
// hand from that table.

#include <cstdio>
#include <string>
#include <vector>

#include "runtime/hle/touch_gestures.h"

namespace {

using recomp::TouchAction;
using recomp::TouchGestures;
using K = TouchAction::Kind;

int g_failures = 0;

std::string Str(const std::vector<TouchAction>& v) {
  std::string s;
  char buf[64];
  for (const TouchAction& a : v) {
    const char* k = a.kind == K::Move ? "M" : a.kind == K::Down ? "D" : "U";
    if (a.kind == K::Move)
      std::snprintf(buf, sizeof buf, "%s(%g,%g) ", k, a.x, a.y);
    else
      std::snprintf(buf, sizeof buf, "%s%d(%g,%g)x%d ", k, a.button, a.x, a.y, a.clicks);
    s += buf;
  }
  return s;
}

void Expect(TouchGestures& g, const std::string& want, const char* what) {
  const std::string got = Str(g.Take());
  if (got != want) {
    std::printf("FAIL: %s\n  want: %s\n  got:  %s\n", what, want.c_str(), got.c_str());
    g_failures++;
  }
}

}  // namespace

int main() {
  {  // Tap: hover on landing, click on lifting (within the slop).
    TouchGestures g;
    g.Down(1, 100, 200, 1000);
    Expect(g, "M(100,200) ", "tap: hover");
    g.Move(1, 105, 203, 1050);
    g.Up(1, 105, 203, 1100);
    Expect(g, "M(105,203) D1(105,203)x1 U1(105,203)x1 ", "tap: click");
  }
  {  // Drag: left down where the finger landed once it moves past the slop.
    TouchGestures g;
    g.Down(1, 100, 100, 0);
    g.Move(1, 110, 100, 10);  // 10 <= 12: still a tap
    Expect(g, "M(100,100) ", "drag: within slop");
    g.Move(1, 130, 100, 20);
    g.Move(1, 160, 140, 30);
    g.Up(1, 160, 140, 40);
    Expect(g, "D1(100,100)x1 M(130,100) M(160,140) U1(160,140)x1 ", "drag");
    g.Tick(5000);  // no hold after a drag
    Expect(g, "", "drag: no hold");
  }
  {  // Hold still: a right click at the landing point after kHoldMs, once.
    TouchGestures g;
    g.Down(1, 50, 60, 1000);
    g.Take();
    g.Tick(1499);
    Expect(g, "", "hold: not yet");
    g.Tick(1500);
    Expect(g, "M(50,60) D3(50,60)x1 U3(50,60)x1 ", "hold: right click");
    g.Tick(3000);
    g.Up(1, 50, 60, 3100);
    Expect(g, "", "hold: nothing on lift");
  }
  {  // Two-finger tap: a right click at the centre.
    TouchGestures g;
    g.Down(1, 100, 100, 0);
    g.Down(2, 200, 300, 50);
    g.Up(1, 100, 100, 200);
    g.Up(2, 200, 300, 220);
    Expect(g, "M(100,100) M(150,200) D3(150,200)x1 U3(150,200)x1 ", "two-finger tap");
  }
  {  // Two fingers held too long: no click.
    TouchGestures g;
    g.Down(1, 100, 100, 0);
    g.Down(2, 200, 300, 50);
    g.Tick(1000);  // not a one-finger hold
    g.Up(1, 100, 100, 600);
    g.Up(2, 200, 300, 610);
    Expect(g, "M(100,100) ", "two-finger long press");
  }
  {  // Two-finger drag: a right-button drag of the centre.
    TouchGestures g;
    g.Down(1, 100, 100, 0);
    g.Down(2, 200, 100, 10);  // centre (150,100)
    g.Take();
    g.Move(1, 120, 100, 20);   // centre (160,100): 10, within the slop
    Expect(g, "", "two-finger drag: within slop");
    g.Move(2, 230, 100, 30);   // centre (175,100)
    g.Move(1, 140, 120, 40);   // centre (185,110)
    g.Up(2, 230, 100, 50);     // centre before lifting (185,110)
    g.Up(1, 140, 120, 60);
    Expect(g, "M(150,100) D3(150,100)x1 M(175,100) M(185,110) U3(185,110)x1 ",
           "two-finger drag");
  }
  {  // Pan: the distance moved is owed; the pointer is held 10 px off the
     // press point against it while scrolling at 0.5 px/ms pays it.
    TouchGestures g;
    g.SetPan(10, 0.5f);
    g.Down(1, 100, 100, 0);
    g.Down(2, 200, 100, 0);    // centre (150,100)
    g.Take();
    g.Move(1, 130, 100, 100);  // centre (165,100): 15 px owed
    Expect(g, "M(150,100) D3(150,100)x1 M(140,100) ", "pan: press, then off against the motion");
    g.Tick(110);               // 5 paid, 10 owed
    Expect(g, "", "pan: still owed");
    g.Move(1, 130, 140, 130);  // 10 paid -> 0 owed; then 20 down owed
    Expect(g, "M(150,90) ", "pan: owed downwards");
    g.Tick(160);               // 15 paid, 5 owed
    Expect(g, "", "pan: 5 owed");
    g.Tick(170);               // paid off
    Expect(g, "M(150,100) ", "pan: back on the press point");
    g.Tick(400);
    Expect(g, "", "pan: nothing owed");
    g.Move(2, 260, 100, 500);  // centre (195,120): 30 right owed
    Expect(g, "M(140,100) ", "pan: owed again");
    g.Up(1, 130, 140, 510);    // the rest is dropped
    g.Up(2, 260, 100, 520);
    Expect(g, "M(150,100) U3(150,100)x1 ", "pan: release at the press point");
  }
  {  // Double tap: the second click says so; a third tap starts over.
    TouchGestures g;
    g.Down(1, 100, 100, 0);
    g.Up(1, 100, 100, 50);
    g.Down(1, 110, 105, 200);
    g.Up(1, 110, 105, 250);
    g.Down(1, 110, 105, 400);
    g.Up(1, 110, 105, 450);
    Expect(g,
           "M(100,100) M(100,100) D1(100,100)x1 U1(100,100)x1 "
           "M(110,105) M(110,105) D1(110,105)x2 U1(110,105)x2 "
           "M(110,105) M(110,105) D1(110,105)x1 U1(110,105)x1 ",
           "double tap");
  }
  {  // Taps too far apart in place are not a double click.
    TouchGestures g;
    g.Down(1, 100, 100, 0);
    g.Up(1, 100, 100, 50);
    g.Down(1, 200, 100, 100);
    g.Up(1, 200, 100, 150);
    Expect(g,
           "M(100,100) M(100,100) D1(100,100)x1 U1(100,100)x1 "
           "M(200,100) M(200,100) D1(200,100)x1 U1(200,100)x1 ",
           "two separate taps");
  }
  if (g_failures) return 1;
  std::printf("touch_gestures: all passed\n");
  return 0;
}
