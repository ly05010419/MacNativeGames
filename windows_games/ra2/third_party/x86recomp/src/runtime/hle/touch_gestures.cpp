#include "runtime/hle/touch_gestures.h"

#include <algorithm>
#include <cmath>

namespace recomp {
namespace {

constexpr int kLeft = 1, kRight = 3;

float Dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

}  // namespace

void TouchGestures::Emit(TouchAction::Kind kind, int button, float x, float y, int clicks) {
  out_.push_back({kind, button, x, y, clicks});
}

void TouchGestures::Centre(float* x, float* y) const {
  float sx = 0, sy = 0;
  for (const auto& [id, f] : fingers_) sx += f.x, sy += f.y;
  const float n = fingers_.empty() ? 1.0f : float(fingers_.size());
  *x = sx / n;
  *y = sy / n;
}

void TouchGestures::Click(int button, float x, float y, uint64_t ms) {
  int clicks = 1;
  if (button == kLeft) {
    if (ms - last_click_ms_ <= kDoubleMs && Dist(x, y, last_x_, last_y_) <= kDoubleSlop) {
      clicks = 2;
      last_click_ms_ = 0;  // a third tap starts over
    } else {
      last_click_ms_ = ms;
    }
    last_x_ = x;
    last_y_ = y;
  }
  Emit(TouchAction::Kind::Move, 0, x, y);
  Emit(TouchAction::Kind::Down, button, x, y, clicks);
  Emit(TouchAction::Kind::Up, button, x, y, clicks);
}

void TouchGestures::Down(uint64_t id, float x, float y, uint64_t ms) {
  fingers_[id] = {x, y, x, y};
  switch (state_) {
    case State::Idle:
      state_ = State::Pending1;
      t0_ = ms;
      Emit(TouchAction::Kind::Move, 0, x, y);  // hover where it landed
      break;
    case State::Pending1:
      state_ = State::Pending2;
      t0_ = ms;
      Centre(&cx0_, &cy0_);
      break;
    default:
      break;  // a finger added to a gesture under way changes nothing
  }
}

void TouchGestures::Move(uint64_t id, float x, float y, uint64_t ms) {
  (void)ms;
  auto it = fingers_.find(id);
  if (it == fingers_.end()) return;
  it->second.x = x;
  it->second.y = y;
  const Finger& f = it->second;
  switch (state_) {
    case State::Pending1:
      if (Dist(x, y, f.x0, f.y0) > kSlop) {
        // The drag starts where the finger landed (the pointer is there).
        state_ = State::Drag1;
        Emit(TouchAction::Kind::Down, kLeft, f.x0, f.y0);
        Emit(TouchAction::Kind::Move, 0, x, y);
      }
      break;
    case State::Drag1:
      Emit(TouchAction::Kind::Move, 0, x, y);
      break;
    case State::Pending2: {
      float cx, cy;
      Centre(&cx, &cy);
      if (Dist(cx, cy, cx0_, cy0_) > kSlop) {
        state_ = State::Drag2;
        Emit(TouchAction::Kind::Move, 0, cx0_, cy0_);
        Emit(TouchAction::Kind::Down, kRight, cx0_, cy0_);
        if (Panning()) {
          px_ = cx0_;
          py_ = cy0_;
          debt_x_ = debt_y_ = 0;
          pt_ = ms;
          pan_off_ = false;
          ptr_x_ = cx0_;
          ptr_y_ = cy0_;
          debt_x_ += cx - px_;
          debt_y_ += cy - py_;
          px_ = cx;
          py_ = cy;
          PanPlace();
        } else {
          Emit(TouchAction::Kind::Move, 0, cx, cy);
        }
      }
      break;
    }
    case State::Drag2: {
      float cx, cy;
      Centre(&cx, &cy);
      if (!Panning()) {
        Emit(TouchAction::Kind::Move, 0, cx, cy);
        break;
      }
      PanPay(ms);  // pay for the time so far, then owe the new motion
      debt_x_ += cx - px_;
      debt_y_ += cy - py_;
      px_ = cx;
      py_ = cy;
      PanPlace();
      break;
    }
    default:
      break;
  }
}

void TouchGestures::Up(uint64_t id, float x, float y, uint64_t ms) {
  auto it = fingers_.find(id);
  if (it == fingers_.end()) return;
  it->second.x = x;
  it->second.y = y;
  float cx, cy;
  Centre(&cx, &cy);
  fingers_.erase(it);
  switch (state_) {
    case State::Pending1:
      Click(kLeft, x, y, ms);
      state_ = State::Idle;
      break;
    case State::Drag1:
      Emit(TouchAction::Kind::Up, kLeft, x, y);
      state_ = fingers_.empty() ? State::Idle : State::Done;
      break;
    case State::Pending2:
      if (ms - t0_ <= kTwoTapMs) Click(kRight, cx, cy, ms);
      state_ = State::Done;
      break;
    case State::Drag2:
      if (Panning()) {
        // What is still owed is dropped: the view stops with the fingers.
        if (pan_off_) Emit(TouchAction::Kind::Move, 0, cx0_, cy0_);
        Emit(TouchAction::Kind::Up, kRight, cx0_, cy0_);
        pan_off_ = false;
      } else {
        Emit(TouchAction::Kind::Up, kRight, cx, cy);
      }
      state_ = State::Done;
      break;
    case State::Held:
    case State::Done:
    case State::Idle:
      break;
  }
  if (fingers_.empty()) state_ = State::Idle;
}

void TouchGestures::Tick(uint64_t ms) {
  if (state_ == State::Drag2 && Panning()) {
    PanPay(ms);
    PanPlace();
    return;
  }
  if (state_ != State::Pending1 || ms - t0_ < kHoldMs || fingers_.empty()) return;
  const Finger& f = fingers_.begin()->second;
  Click(kRight, f.x0, f.y0, ms);
  state_ = State::Held;
}

// Pays what is owed for the time since the last payment (the view
// scrolled at pan_speed_ while the pointer was off).
void TouchGestures::PanPay(uint64_t ms) {
  const float len = std::hypot(debt_x_, debt_y_);
  if (pan_off_ && len > 0) {
    const float paid = pan_speed_ * float(ms - pt_);
    if (paid >= len) {
      debt_x_ = debt_y_ = 0;
    } else {
      debt_x_ *= (len - paid) / len;
      debt_y_ *= (len - paid) / len;
    }
  }
  pt_ = ms;
}

// Puts the pointer off the press point against what is owed, or back on
// it once (nearly) nothing is.
void TouchGestures::PanPlace() {
  const float len = std::hypot(debt_x_, debt_y_);
  float x = cx0_, y = cy0_;
  pan_off_ = len > kPanEpsilon;
  if (pan_off_) {
    x -= debt_x_ / len * pan_offset_;
    y -= debt_y_ / len * pan_offset_;
  } else {
    debt_x_ = debt_y_ = 0;
  }
  if (x != ptr_x_ || y != ptr_y_) {
    ptr_x_ = x;
    ptr_y_ = y;
    Emit(TouchAction::Kind::Move, 0, x, y);
  }
}

std::vector<TouchAction> TouchGestures::Take() {
  std::vector<TouchAction> r;
  r.swap(out_);
  return r;
}

}  // namespace recomp
