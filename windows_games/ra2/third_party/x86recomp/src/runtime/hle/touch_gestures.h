#pragma once
// Touch-screen gestures as mouse input, for games played with a mouse on
// a touch screen (GameConfig::Touch::Pointer). Pure logic: finger events
// in (window coordinates, milliseconds), mouse actions out.
//
//   one finger, tap                 left click where it lifted
//   one finger, drag                left-button drag from where it landed
//   one finger, held still kHoldMs  right click
//   two fingers, tap                right click at their centre
//   two fingers, drag               right-button drag of their centre
//
// Games that scroll while the right button is held, at a speed set by
// how far the pointer is from where it was pressed, get a pan instead
// (SetPan): the distance the fingers move is owed to the view, and the
// pointer is held `offset` off the press point (against the fingers, so
// the view follows them) for as long as scrolling at `speed` takes to
// pay it, then put back.
//
// The pointer follows a finger that lands (hover), so a tap is a click
// where the game has already seen the pointer. Two taps close together in
// time and place make a double click.

#include <cstdint>
#include <map>
#include <vector>

namespace recomp {

struct TouchAction {
  enum class Kind { Move, Down, Up } kind;
  int button;  // 1 left, 3 right (SDL numbering); 0 for Move
  float x, y;
  int clicks;  // Down/Up: 1, or 2 for the second click of a double click
};

class TouchGestures {
 public:
  static constexpr float kSlop = 12.0f;          // movement that makes a drag
  static constexpr uint64_t kHoldMs = 500;       // press-and-hold right click
  static constexpr uint64_t kTwoTapMs = 350;     // two-finger tap: lifted within
  static constexpr uint64_t kDoubleMs = 400;     // double-tap window
  static constexpr float kDoubleSlop = 24.0f;

  static constexpr float kPanEpsilon = 2.0f;     // owed distance that is paid

  // offset: how far off the press point the pointer is held while
  // panning; speed: how fast the game scrolls then (px/ms). offset 0: a
  // plain right-button drag.
  void SetPan(float offset, float speed) {
    pan_offset_ = offset;
    pan_speed_ = speed;
  }

  void Down(uint64_t id, float x, float y, uint64_t ms);
  void Move(uint64_t id, float x, float y, uint64_t ms);
  void Up(uint64_t id, float x, float y, uint64_t ms);
  // Fires the press-and-hold right click; call regularly.
  void Tick(uint64_t ms);
  // The actions produced since the last call, in order.
  std::vector<TouchAction> Take();

 private:
  enum class State { Idle, Pending1, Drag1, Held, Pending2, Drag2, Done };
  struct Finger {
    float x, y, x0, y0;
  };
  void Centre(float* x, float* y) const;
  void Click(int button, float x, float y, uint64_t ms);
  void Emit(TouchAction::Kind kind, int button, float x, float y, int clicks = 1);

  State state_ = State::Idle;
  std::map<uint64_t, Finger> fingers_;
  uint64_t t0_ = 0;          // when the gesture began
  float cx0_ = 0, cy0_ = 0;  // two fingers: their centre at the start
  void PanPay(uint64_t ms);
  void PanPlace();
  bool Panning() const { return pan_offset_ > 0; }
  // Panning: the last centre; the distance owed to the view and when it
  // was last paid; the pointer's place (off the press point or not).
  float pan_offset_ = 0, pan_speed_ = 0;
  float px_ = 0, py_ = 0;
  float debt_x_ = 0, debt_y_ = 0;
  uint64_t pt_ = 0;
  bool pan_off_ = false;
  float ptr_x_ = 0, ptr_y_ = 0;
  // The last left click, for double clicks.
  uint64_t last_click_ms_ = 0;
  float last_x_ = -1e9f, last_y_ = -1e9f;
  std::vector<TouchAction> out_;
};

}  // namespace recomp
