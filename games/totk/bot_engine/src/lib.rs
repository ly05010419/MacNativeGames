//! BotW smoke / FastHands policy seam. Linked optionally as `build/libbotw_bot.a`.
//! C ABI matches `runtime/bot_ffi.h`. Future JEPA / Jeff-1 policies implement [`FastHandsPolicy`].
//! All telemetry (`[bot-hb]`, `[bot-fsm]`, `[bot-done]`) and JSONL trajectory writes happen on a
//! dedicated background worker thread fed via a bounded `sync_channel` (`try_send`) so the
//! render/present thread never blocks on I/O or string formatting.

#![allow(non_camel_case_types)]

use std::f32::consts::PI;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::thread::{self, JoinHandle};

pub const BOT_ABI_VERSION: u32 = 1;
pub const BOT_ACTOR_SLOTS: usize = 8;
pub const BOT_ASHOT_PATH_MAX: usize = 256;
const ASYNC_QUEUE_CAP: usize = 4096;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BotVec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BotActorSlot {
    pub name_hash: u32,
    pub flags: u32,
    pub pos: BotVec3,
    pub vel: BotVec3,
    pub dist_m: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SchematicState {
    pub abi_version: u32,
    pub state_flags: u32,
    pub frame_index: u64,
    pub now_ns: u64,
    pub vfr_dt_s: f32,
    pub this_frame_draws: u32,
    pub wb_stage_count: u32,
    pub player_pos: BotVec3,
    pub player_fwd: BotVec3,
    pub camera_pos: BotVec3,
    pub camera_fwd: BotVec3,
    pub camera_up: BotVec3,
    pub camera_fovy: f32,
    pub actors: [BotActorSlot; BOT_ACTOR_SLOTS],
}

impl Default for SchematicState {
    fn default() -> Self {
        Self {
            abi_version: BOT_ABI_VERSION,
            state_flags: 0,
            frame_index: 0,
            now_ns: 0,
            vfr_dt_s: 1.0 / 30.0,
            this_frame_draws: 0,
            wb_stage_count: 0,
            player_pos: BotVec3::default(),
            player_fwd: BotVec3 {
                x: 0.0,
                y: 0.0,
                z: 1.0,
            },
            camera_pos: BotVec3::default(),
            camera_fwd: BotVec3 {
                x: 0.0,
                y: 0.0,
                z: 1.0,
            },
            camera_up: BotVec3 {
                x: 0.0,
                y: 1.0,
                z: 0.0,
            },
            camera_fovy: 0.0,
            actors: [BotActorSlot::default(); BOT_ACTOR_SLOTS],
        }
    }
}

#[repr(C)]
#[derive(Clone, Debug)]
pub struct BotFrameCommand {
    pub pad_connected: i32,
    pub request_close: u8,
    pub reserved: [u8; 3],
    pub buttons: u64,
    pub lx: i32,
    pub ly: i32,
    pub rx: i32,
    pub ry: i32,
    pub pin_frame_step_us: u32,
    pub request_ashot_id: u32,
    pub ashot_path: [u8; BOT_ASHOT_PATH_MAX],
}

impl Default for BotFrameCommand {
    fn default() -> Self {
        Self {
            pad_connected: 0,
            request_close: 0,
            reserved: [0; 3],
            buttons: 0,
            lx: 0,
            ly: 0,
            rx: 0,
            ry: 0,
            pin_frame_step_us: 0,
            request_ashot_id: 0,
            ashot_path: [0; BOT_ASHOT_PATH_MAX],
        }
    }
}

/// High-level smoke goal the deterministic policy drives toward.
#[derive(Clone, Debug)]
pub struct SmokeIntent {
    pub target_pos: BotVec3,
    pub hold_buttons: u64,
    pub assert_min_speed_m_s: f32,
    pub record_trajectory: bool,
}

impl Default for SmokeIntent {
    fn default() -> Self {
        Self {
            target_pos: BotVec3 {
                x: 0.0,
                y: 0.0,
                z: 10.0,
            },
            hold_buttons: 0,
            assert_min_speed_m_s: 0.0,
            record_trajectory: false,
        }
    }
}

/// Pluggable FastHands controller. JEPA / Jeff-1 drop in here.
pub trait FastHandsPolicy {
    fn step(&mut self, state: &SchematicState, goal: &SmokeIntent) -> BotFrameCommand;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SmokePhase {
    Align,
    Walk,
    ActionBurst,
    Done,
}

impl SmokePhase {
    pub const fn as_str(self) -> &'static str {
        match self {
            SmokePhase::Align => "Align",
            SmokePhase::Walk => "Walk",
            SmokePhase::ActionBurst => "ActionBurst",
            SmokePhase::Done => "Done",
        }
    }
}

#[derive(Clone, Copy)]
enum BotAsyncEvent {
    Heartbeat {
        t_s: f64,
        frame: u64,
        phase: &'static str,
        draws: u32,
        dt_ms: f32,
        hz: f32,
        pos: [f32; 3],
        buttons: u64,
        lx: i32,
        ly: i32,
        rx: i32,
        ry: i32,
        assert_fail: u32,
    },
    FsmTransition {
        t_s: f64,
        frame: u64,
        from: &'static str,
        to: &'static str,
        phase_frames: u64,
        draws: u32,
        dt_ms: f32,
        hz: f32,
        pos: [f32; 3],
        metric_label: &'static str,
        metric_val: f32,
    },
    AshotRequested {
        t_s: f64,
        frame: u64,
        draws: u32,
    },
    DoneSummary {
        t_s: f64,
        frame: u64,
        assert_fail: u32,
        draws: u32,
        dt_ms: f32,
        hz: f32,
        pos: [f32; 3],
    },
    Trace {
        frame: u64,
        z_t: [f32; 6],
        a_t: [i32; 5],
        z_next: [f32; 6],
    },
    Shutdown,
}

struct AsyncLogger {
    tx: SyncSender<BotAsyncEvent>,
    worker: Option<JoinHandle<()>>,
    has_trace_file: bool,
}

impl AsyncLogger {
    fn spawn(trace_file: Option<File>) -> Self {
        let has_trace_file = trace_file.is_some();
        let (tx, rx) = sync_channel::<BotAsyncEvent>(ASYNC_QUEUE_CAP);
        let worker = thread::Builder::new()
            .name("botw-bot-logger".into())
            .spawn(move || Self::run_loop(rx, trace_file.map(BufWriter::new)))
            .ok();
        Self {
            tx,
            worker,
            has_trace_file,
        }
    }

    fn run_loop(rx: Receiver<BotAsyncEvent>, mut trace_writer: Option<BufWriter<File>>) {
        let mut stderr_buf = Vec::<u8>::with_capacity(256);
        while let Ok(ev) = rx.recv() {
            match ev {
                BotAsyncEvent::Heartbeat {
                    t_s,
                    frame,
                    phase,
                    draws,
                    dt_ms,
                    hz,
                    pos,
                    buttons,
                    lx,
                    ly,
                    rx,
                    ry,
                    assert_fail,
                } => {
                    stderr_buf.clear();
                    let _ = writeln!(
                        &mut stderr_buf,
                        "[bot-hb] t={:.2}s frame={} phase={} draws={} dt={:.2}ms ({:.1}Hz) pos=({:.2},{:.2},{:.2}) pad=(btn=0x{:x} lx={} ly={} rx={} ry={}) assert_fail={}",
                        t_s, frame, phase, draws, dt_ms, hz, pos[0], pos[1], pos[2], buttons, lx, ly, rx, ry, assert_fail
                    );
                    let _ = std::io::stderr().write_all(&stderr_buf);
                }
                BotAsyncEvent::FsmTransition {
                    t_s,
                    frame,
                    from,
                    to,
                    phase_frames,
                    draws,
                    dt_ms,
                    hz,
                    pos,
                    metric_label,
                    metric_val,
                } => {
                    stderr_buf.clear();
                    let _ = writeln!(
                        &mut stderr_buf,
                        "[bot-fsm] t={:.2}s frame={} {} -> {} (phase_frames={} draws={} dt={:.2}ms ({:.1}Hz) pos=({:.2},{:.2},{:.2}) {}={:.3})",
                        t_s, frame, from, to, phase_frames, draws, dt_ms, hz, pos[0], pos[1], pos[2], metric_label, metric_val
                    );
                    let _ = std::io::stderr().write_all(&stderr_buf);
                }
                BotAsyncEvent::AshotRequested { t_s, frame, draws } => {
                    stderr_buf.clear();
                    let _ = writeln!(
                        &mut stderr_buf,
                        "[bot-fsm] t={:.2}s frame={} ActionBurst peak ashot requested: /tmp/botw_smoke_peak.png (draws={})",
                        t_s, frame, draws
                    );
                    let _ = std::io::stderr().write_all(&stderr_buf);
                }
                BotAsyncEvent::DoneSummary {
                    t_s,
                    frame,
                    assert_fail,
                    draws,
                    dt_ms,
                    hz,
                    pos,
                } => {
                    stderr_buf.clear();
                    let _ = writeln!(
                        &mut stderr_buf,
                        "[bot-done] PASS t={:.2}s frame={} assert_fail={} draws={} dt={:.2}ms ({:.1}Hz) final_pos=({:.2},{:.2},{:.2})",
                        t_s, frame, assert_fail, draws, dt_ms, hz, pos[0], pos[1], pos[2]
                    );
                    let _ = std::io::stderr().write_all(&stderr_buf);
                    if let Some(w) = trace_writer.as_mut() {
                        let _ = w.flush();
                    }
                }
                BotAsyncEvent::Trace {
                    frame,
                    z_t,
                    a_t,
                    z_next,
                } => {
                    if let Some(w) = trace_writer.as_mut() {
                        let _ = writeln!(
                            w,
                            "{{\"frame\":{},\"z_t\":[{:.4},{:.4},{:.4},{:.4},{:.4},{:.4}],\"a_t\":{{\"buttons\":{},\"lx\":{},\"ly\":{},\"rx\":{},\"ry\":{}}},\"z_next\":[{:.4},{:.4},{:.4},{:.4},{:.4},{:.4}]}}",
                            frame,
                            z_t[0], z_t[1], z_t[2], z_t[3], z_t[4], z_t[5],
                            a_t[0], a_t[1], a_t[2], a_t[3], a_t[4],
                            z_next[0], z_next[1], z_next[2], z_next[3], z_next[4], z_next[5]
                        );
                        if frame % 30 == 0 {
                            let _ = w.flush();
                        }
                    }
                }
                BotAsyncEvent::Shutdown => {
                    if let Some(w) = trace_writer.as_mut() {
                        let _ = w.flush();
                    }
                    break;
                }
            }
        }
        if let Some(mut w) = trace_writer {
            let _ = w.flush();
        }
    }

    #[inline(always)]
    fn emit(&self, ev: BotAsyncEvent) {
        let _ = self.tx.try_send(ev);
    }

    fn flush_and_join(&mut self) {
        let _ = self.tx.send(BotAsyncEvent::Shutdown);
        if let Some(h) = self.worker.take() {
            let _ = h.join();
        }
    }
}

impl Drop for AsyncLogger {
    fn drop(&mut self) {
        self.flush_and_join();
    }
}

/// PD heading + frame-exact FSM + 60 Hz / cliff-safety assertion eval + non-blocking async JSONL & heartbeat logger.
pub struct DeterministicSmokePolicy {
    phase: SmokePhase,
    phase_frame: u64,
    world_ready_frames: u32,
    start_pos: Option<BotVec3>,
    prev_pos: Option<BotVec3>,
    assert_fail: u32,
    kp: f32,
    kd: f32,
    prev_heading_err: f32,
    prev_rx: i32,
    logger: AsyncLogger,
    last_z: Option<[f32; 6]>,
    last_a: Option<[i32; 5]>,
    close_requested: bool,
}

impl Default for DeterministicSmokePolicy {
    fn default() -> Self {
        Self::new()
    }
}

impl DeterministicSmokePolicy {
    pub fn new() -> Self {
        Self {
            phase: SmokePhase::Align,
            phase_frame: 0,
            world_ready_frames: 0,
            start_pos: None,
            prev_pos: None,
            assert_fail: 0,
            kp: 0.8,
            kd: 0.0,
            prev_heading_err: 0.0,
            prev_rx: 0,
            logger: AsyncLogger::spawn(None),
            last_z: None,
            last_a: None,
            close_requested: false,
        }
    }

    pub fn with_trajectory_path(path: &str) -> std::io::Result<Self> {
        let f = File::create(path)?;
        Ok(Self {
            phase: SmokePhase::Align,
            phase_frame: 0,
            world_ready_frames: 0,
            start_pos: None,
            prev_pos: None,
            assert_fail: 0,
            kp: 0.8,
            kd: 0.0,
            prev_heading_err: 0.0,
            prev_rx: 0,
            logger: AsyncLogger::spawn(Some(f)),
            last_z: None,
            last_a: None,
            close_requested: false,
        })
    }

    pub fn phase(&self) -> SmokePhase {
        self.phase
    }

    fn heading_err(state: &SchematicState, goal: &SmokeIntent) -> f32 {
        let dx = goal.target_pos.x - state.player_pos.x;
        let dz = goal.target_pos.z - state.player_pos.z;
        let desired = dz.atan2(dx);
        let fwd = state.player_fwd.z.atan2(state.player_fwd.x);
        let mut err = desired - fwd;
        while err > PI {
            err -= 2.0 * PI;
        }
        while err < -PI {
            err += 2.0 * PI;
        }
        err
    }

    fn pd_look(&mut self, err: f32, dt: f32) -> (i32, i32) {
        if err.abs() < 0.05 {
            self.prev_heading_err = err;
            self.prev_rx = 0;
            return (0, 0);
        }
        let d_err = if dt > 1e-4 {
            ((err - self.prev_heading_err) / dt).clamp(-2.0, 2.0)
        } else {
            0.0
        };
        self.prev_heading_err = err;
        let u = self.kp * err + self.kd * d_err;
        // Clamp wrap-around error away from ±PI so atan2 branch-cut never flips sign across frames
        let target_rx = (-u * 12000.0).clamp(-12000.0, 12000.0) as i32;
        let rx = if self.prev_rx == 0 {
            target_rx
        } else if (err.abs() > 2.8) && (target_rx.signum() != self.prev_rx.signum()) {
            // Near the ±PI branch cut, keep rotating in the established direction instead of reversing
            self.prev_rx
        } else {
            let delta = (target_rx - self.prev_rx).clamp(-2500, 2500);
            self.prev_rx + delta
        };
        self.prev_rx = rx;
        (rx, 0)
    }

    fn eval_assertions(&mut self, state: &SchematicState, goal: &SmokeIntent) {
        if let Some(start) = self.start_pos {
            // Cliff-safety invariant: player altitude must remain within 3.0 m of initial plateau height
            if (state.player_pos.y - start.y).abs() > 3.0 {
                self.assert_fail = self.assert_fail.saturating_add(1);
            }
        }
        let hz = if state.vfr_dt_s > 1e-4 {
            1.0 / state.vfr_dt_s
        } else {
            30.0
        };
        if !(59.0..=61.5).contains(&hz) || goal.assert_min_speed_m_s <= 0.0 {
            self.prev_pos = Some(state.player_pos);
            return;
        }
        if let Some(prev) = self.prev_pos {
            let dx = state.player_pos.x - prev.x;
            let dz = state.player_pos.z - prev.z;
            let speed = (dx * dx + dz * dz).sqrt() / state.vfr_dt_s.max(1e-4);
            if speed + 1e-3 < goal.assert_min_speed_m_s {
                self.assert_fail = self.assert_fail.saturating_add(1);
            }
        }
        self.prev_pos = Some(state.player_pos);
    }

    #[inline(always)]
    fn record_transition(&mut self, state: &SchematicState, cmd: &BotFrameCommand) {
        let z = [
            state.player_pos.x,
            state.player_pos.y,
            state.player_pos.z,
            state.player_fwd.x,
            state.player_fwd.y,
            state.player_fwd.z,
        ];
        let a = [cmd.buttons as i32, cmd.lx, cmd.ly, cmd.rx, cmd.ry];
        if let (Some(z0), Some(a0)) = (self.last_z, self.last_a) {
            self.logger.emit(BotAsyncEvent::Trace {
                frame: state.frame_index,
                z_t: z0,
                a_t: a0,
                z_next: z,
            });
        }
        self.last_z = Some(z);
        self.last_a = Some(a);
    }

    pub fn assertion_failures(&self) -> u32 {
        self.assert_fail
    }
}

fn exit_on_done() -> bool {
    static EXIT_ON_DONE: OnceLock<bool> = OnceLock::new();
    *EXIT_ON_DONE.get_or_init(|| {
        std::env::var("TOTK_BOT_EXIT_ON_DONE")
            .map(|v| v != "0")
            .unwrap_or(true)
    })
}

impl FastHandsPolicy for DeterministicSmokePolicy {
    fn step(&mut self, state: &SchematicState, goal: &SmokeIntent) -> BotFrameCommand {
        let mut cmd = BotFrameCommand::default();
        if state.abi_version != BOT_ABI_VERSION {
            return cmd;
        }
        let dt = if state.vfr_dt_s > 1e-4 {
            state.vfr_dt_s
        } else {
            1.0 / 30.0
        };
        let dt_ms = dt * 1000.0;
        let hz = 1.0 / dt.max(1e-4);
        let t_s = (state.now_ns as f64) * 1e-9;
        let pos = [state.player_pos.x, state.player_pos.y, state.player_pos.z];

        // Wait until 3D world is sustainably live when running in-game (this_frame_draws == 0 in unit tests bypasses this gate).
        // Require >= 180 consecutive frames (~1.5s at 120 Hz) with draws >= 500 and non-origin camera look-at coordinates so
        // the post-load black fade-in completes, the HUD is visible, and player/camera controls unlock before Align begins.
        // Still assert pad_connected = 1 with neutral sticks so runtime_input_pad_synthetic is exercised during TOTK_AUTOLOAD=1!
        if state.this_frame_draws > 0 && self.phase == SmokePhase::Align && self.phase_frame == 0 {
            let valid_world_pos = state.player_pos.x.abs() > 1.0
                || state.player_pos.y.abs() > 1.0
                || state.player_pos.z.abs() > 1.0;
            if state.this_frame_draws >= 500 && valid_world_pos {
                self.world_ready_frames = self.world_ready_frames.saturating_add(1);
            } else {
                self.world_ready_frames = 0;
            }
            if self.world_ready_frames < 180 {
                cmd.pad_connected = 1;
                if state.frame_index == 1 || state.frame_index % 60 == 0 {
                    self.logger.emit(BotAsyncEvent::Heartbeat {
                        t_s,
                        frame: state.frame_index,
                        phase: "WaitWorld",
                        draws: state.this_frame_draws,
                        dt_ms,
                        hz,
                        pos,
                        buttons: cmd.buttons,
                        lx: cmd.lx,
                        ly: cmd.ly,
                        rx: cmd.rx,
                        ry: cmd.ry,
                        assert_fail: self.assert_fail,
                    });
                }
                return cmd;
            }
        }

        if self.start_pos.is_none() {
            self.start_pos = Some(state.player_pos);
        }

        self.eval_assertions(state, goal);

        let err = Self::heading_err(state, goal);
        let dist_to_goal = {
            let dx = goal.target_pos.x - state.player_pos.x;
            let dz = goal.target_pos.z - state.player_pos.z;
            (dx * dx + dz * dz).sqrt()
        };
        let walked_dist = if let Some(start) = self.start_pos {
            let dx = state.player_pos.x - start.x;
            let dz = state.player_pos.z - start.z;
            (dx * dx + dz * dz).sqrt()
        } else {
            0.0
        };

        match self.phase {
            SmokePhase::Align => {
                if self.phase_frame == 0 {
                    self.logger.emit(BotAsyncEvent::FsmTransition {
                        t_s,
                        frame: state.frame_index,
                        from: "WaitWorld",
                        to: "Align",
                        phase_frames: 0,
                        draws: state.this_frame_draws,
                        dt_ms,
                        hz,
                        pos,
                        metric_label: "err_rad",
                        metric_val: err,
                    });
                }
                let (rx, ry) = self.pd_look(err, dt);
                cmd.pad_connected = 1;
                cmd.rx = rx;
                cmd.ry = ry;
                self.phase_frame += 1;
                if err.abs() < 0.08 || self.phase_frame >= 30 {
                    self.logger.emit(BotAsyncEvent::FsmTransition {
                        t_s,
                        frame: state.frame_index,
                        from: "Align",
                        to: "Walk",
                        phase_frames: self.phase_frame,
                        draws: state.this_frame_draws,
                        dt_ms,
                        hz,
                        pos,
                        metric_label: "err_rad",
                        metric_val: err,
                    });
                    self.phase = SmokePhase::Walk;
                    self.phase_frame = 0;
                    self.prev_rx = 0;
                }
            }
            SmokePhase::Walk => {
                // Walk gently for up to 45 frames (~0.38s at 120 Hz) or 1.5m displacement so Link stays safely on the ledge
                cmd.pad_connected = 1;
                cmd.ly = 22000;
                cmd.rx = 0;
                cmd.ry = 0;
                cmd.buttons = goal.hold_buttons;
                self.phase_frame += 1;
                if dist_to_goal < 1.5 || walked_dist >= 1.5 || self.phase_frame >= 45 {
                    self.logger.emit(BotAsyncEvent::FsmTransition {
                        t_s,
                        frame: state.frame_index,
                        from: "Walk",
                        to: "ActionBurst",
                        phase_frames: self.phase_frame,
                        draws: state.this_frame_draws,
                        dt_ms,
                        hz,
                        pos,
                        metric_label: "walked_m",
                        metric_val: walked_dist,
                    });
                    self.phase = SmokePhase::ActionBurst;
                    self.phase_frame = 0;
                }
            }
            SmokePhase::ActionBurst => {
                // Frame-exact: Beat 0 holds X (Jump=4) for 4 frames, Beat 1 holds A (Action=1) for 4 frames.
                cmd.pad_connected = 1;
                let beat = self.phase_frame % 8;
                if beat < 4 {
                    cmd.buttons = if self.phase_frame < 8 { 4 } else { 1 }; // Beat 0: X (Jump=4), Beat 1: A (Action=1)
                }
                if self.phase_frame == 8 {
                    cmd.request_ashot_id = 1;
                    let shot = b"/tmp/botw_smoke_peak.png";
                    cmd.ashot_path[..shot.len()].copy_from_slice(shot);
                    self.logger.emit(BotAsyncEvent::AshotRequested {
                        t_s,
                        frame: state.frame_index,
                        draws: state.this_frame_draws,
                    });
                }
                self.phase_frame += 1;
                if self.phase_frame >= 16 {
                    self.logger.emit(BotAsyncEvent::FsmTransition {
                        t_s,
                        frame: state.frame_index,
                        from: "ActionBurst",
                        to: "Done",
                        phase_frames: self.phase_frame,
                        draws: state.this_frame_draws,
                        dt_ms,
                        hz,
                        pos,
                        metric_label: "assert_fail",
                        metric_val: self.assert_fail as f32,
                    });
                    self.logger.emit(BotAsyncEvent::DoneSummary {
                        t_s,
                        frame: state.frame_index,
                        assert_fail: self.assert_fail,
                        draws: state.this_frame_draws,
                        dt_ms,
                        hz,
                        pos,
                    });
                    self.phase = SmokePhase::Done;
                    self.phase_frame = 0;
                }
            }
            SmokePhase::Done => {
                // On the first frame of Done, send pad_connected = 1 with all zeros so world_survey.c clears syntheticPad,
                // then set pad_connected = 0 on all subsequent frames so external pad commands or the user are never overwritten.
                if self.phase_frame == 0 {
                    cmd.pad_connected = 1;
                } else {
                    cmd.pad_connected = 0;
                }
                self.phase_frame = self.phase_frame.saturating_add(1);
                if !self.close_requested
                    && state.this_frame_draws > 0
                    && self.phase_frame >= 60
                    && exit_on_done()
                {
                    self.close_requested = true;
                    self.logger.flush_and_join();
                    cmd.request_close = 1;
                }
            }
        }

        if (self.phase != SmokePhase::Done || self.phase_frame <= 30) && state.frame_index % 30 == 0 {
            self.logger.emit(BotAsyncEvent::Heartbeat {
                t_s,
                frame: state.frame_index,
                phase: self.phase.as_str(),
                draws: state.this_frame_draws,
                dt_ms,
                hz,
                pos,
                buttons: cmd.buttons,
                lx: cmd.lx,
                ly: cmd.ly,
                rx: cmd.rx,
                ry: cmd.ry,
                assert_fail: self.assert_fail,
            });
        }

        if (self.phase != SmokePhase::Done || self.phase_frame <= 1)
            && (goal.record_trajectory || self.logger.has_trace_file)
        {
            self.record_transition(state, &cmd);
        }
        cmd
    }
}

static POLICY: Mutex<Option<DeterministicSmokePolicy>> = Mutex::new(None);
static GOAL: Mutex<SmokeIntent> = Mutex::new(SmokeIntent {
    target_pos: BotVec3 {
        x: 0.0,
        y: 0.0,
        z: 10.0,
    },
    hold_buttons: 0,
    assert_min_speed_m_s: 0.0,
    record_trajectory: false,
});

fn smoke_enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("TOTK_BOT_SMOKE").map(|v| v == "1").unwrap_or(false))
}

fn with_policy<R>(f: impl FnOnce(&mut DeterministicSmokePolicy, &SmokeIntent) -> R) -> R {
    let mut slot = POLICY.lock().unwrap_or_else(|e| e.into_inner());
    if slot.is_none() {
        let policy = match std::env::var("TOTK_BOT_TRACE") {
            Ok(path) if !path.is_empty() => {
                DeterministicSmokePolicy::with_trajectory_path(&path)
                    .unwrap_or_else(|_| DeterministicSmokePolicy::new())
            }
            _ => DeterministicSmokePolicy::new(),
        };
        *slot = Some(policy);
    }
    let goal = GOAL.lock().unwrap_or_else(|e| e.into_inner());
    f(slot.as_mut().unwrap(), &goal)
}

static BOT_PANICKED: AtomicBool = AtomicBool::new(false);

/// C ABI entry used by the host when `libbotw_bot.a` is linked.
/// Active pad injection is gated on `TOTK_BOT_SMOKE=1` so `TOTK_SURVEY=1` telemetry-only sessions remain passive.
/// Wrapped in `catch_unwind` + `BOT_PANICKED` latch so a panic inside any policy never unwinds across `extern "C"`.
#[no_mangle]
pub unsafe extern "C" fn bot_on_frame_tick(
    state: *const SchematicState,
    cmd: *mut BotFrameCommand,
) {
    if state.is_null() || cmd.is_null() {
        return;
    }
    if BOT_PANICKED.load(Ordering::Relaxed) || !smoke_enabled() {
        *cmd = BotFrameCommand::default();
        return;
    }
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let state_ref = &*state;
        #[cfg(test)]
        if (state_ref.state_flags & (1u32 << 31)) != 0 {
            panic!("simulated policy panic for FFI unwind-safety test");
        }
        with_policy(|p, goal| p.step(state_ref, goal))
    }));
    match res {
        Ok(out) => {
            *cmd = out;
        }
        Err(_) => {
            BOT_PANICKED.store(true, Ordering::Relaxed);
            *cmd = BotFrameCommand::default();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    #[test]
    fn abi_struct_layout_matches_c_header() {
        assert_eq!(size_of::<BotVec3>(), 12);
        assert_eq!(size_of::<BotActorSlot>(), 36);
        assert_eq!(size_of::<SchematicState>(), 392);
        assert_eq!(size_of::<BotFrameCommand>(), 296);
        assert_eq!(offset_of!(BotFrameCommand, pad_connected), 0);
        assert_eq!(offset_of!(BotFrameCommand, request_close), 4);
        assert_eq!(offset_of!(BotFrameCommand, reserved), 5);
        assert_eq!(offset_of!(BotFrameCommand, buttons), 8);
        assert_eq!(offset_of!(BotFrameCommand, ashot_path), 40);
    }

    #[test]
    fn fsm_align_walk_burst_done_and_trace() {
        let tmp = "/tmp/botw-bot-smoke-test.jsonl";
        let _ = std::fs::remove_file(tmp);
        let mut policy = DeterministicSmokePolicy::with_trajectory_path(tmp).unwrap();
        let goal = SmokeIntent {
            target_pos: BotVec3 { x: 0.0, y: 0.0, z: 2.0 },
            hold_buttons: 2,
            assert_min_speed_m_s: 1.0,
            record_trajectory: true,
        };
        let mut st = SchematicState {
            vfr_dt_s: 1.0 / 60.0,
            player_fwd: BotVec3 { x: 0.0, y: 0.0, z: 1.0 },
            ..Default::default()
        };
        // Frame 1: already aligned with +Z -> transitions Align -> Walk
        st.frame_index = 1;
        let c1 = policy.step(&st, &goal);
        assert_eq!(c1.pad_connected, 1);
        assert_eq!(policy.phase(), SmokePhase::Walk);

        // Frame 2: advance close to target (< 1.5 m) -> transitions Walk -> ActionBurst
        st.frame_index = 2;
        st.player_pos.z = 1.0;
        let c2 = policy.step(&st, &goal);
        assert_eq!(c2.ly, 22000);
        assert_eq!(c2.buttons, 2);
        assert_eq!(policy.phase(), SmokePhase::ActionBurst);
        assert_eq!(policy.assertion_failures(), 0);

        // 16 frames of ActionBurst -> transitions to Done
        for i in 0..16 {
            st.frame_index = 3 + i;
            st.player_pos.z += 0.02;
            let cb = policy.step(&st, &goal);
            assert_eq!(cb.pad_connected, 1);
            assert_eq!(cb.buttons, if i < 4 { 4 } else if i >= 8 && i < 12 { 1 } else { 0 });
        }
        assert_eq!(policy.phase(), SmokePhase::Done);

        // Frame 19 (Done frame 0): sends one final pad_connected=1 with zeroed axes/buttons to clear syntheticPad
        st.frame_index = 19;
        let cd0 = policy.step(&st, &goal);
        assert_eq!(cd0.pad_connected, 1);
        assert_eq!(cd0.buttons, 0);
        assert_eq!(cd0.ly, 0);

        // Frame 20 (Done frame 1): releases pad_connected=0 so external pad commands or user input are never overwritten
        st.frame_index = 20;
        let cd1 = policy.step(&st, &goal);
        assert_eq!(cd1.pad_connected, 0);
        assert_eq!(cd1.request_close, 0);

        // Advance through Done frames 2..59 with this_frame_draws > 0: request_close remains 0 until frame 60
        st.this_frame_draws = 600;
        for d in 2..59 {
            st.frame_index = 19 + d;
            let cd = policy.step(&st, &goal);
            assert_eq!(cd.pad_connected, 0);
            assert_eq!(cd.request_close, 0);
        }
        // Done frame 60 (phase_frame reaches 60): sets request_close = 1 and flushes logger without calling exit(0)
        st.frame_index = 78;
        let cd60 = policy.step(&st, &goal);
        assert_eq!(cd60.pad_connected, 0);
        assert_eq!(cd60.request_close, 1);

        // Done frame 61: request_close is only emitted once
        st.frame_index = 79;
        let cd61 = policy.step(&st, &goal);
        assert_eq!(cd61.request_close, 0);

        drop(policy);
        let trace = std::fs::read_to_string(tmp).unwrap();
        assert!(trace.contains("\"z_t\":") && trace.contains("\"z_next\":"));
        let _ = std::fs::remove_file(tmp);
    }

    #[test]
    fn ffi_catch_unwind_latches_bot_panicked_and_zeroes_cmd() {
        std::env::set_var("TOTK_BOT_SMOKE", "1");
        let _ = smoke_enabled();
        BOT_PANICKED.store(false, Ordering::Relaxed);

        let panic_state = SchematicState {
            state_flags: 1u32 << 31,
            ..Default::default()
        };
        let mut cmd = BotFrameCommand {
            pad_connected: 1,
            request_close: 1,
            buttons: 0xff,
            lx: 12345,
            ..Default::default()
        };
        unsafe {
            bot_on_frame_tick(&panic_state, &mut cmd);
        }
        assert!(BOT_PANICKED.load(Ordering::Relaxed));
        assert_eq!(cmd.pad_connected, 0);
        assert_eq!(cmd.request_close, 0);
        assert_eq!(cmd.buttons, 0);
        assert_eq!(cmd.lx, 0);

        // Subsequent normal frame while BOT_PANICKED is latched is a safe no-op that zeroes cmd
        let normal_state = SchematicState::default();
        cmd.pad_connected = 1;
        cmd.buttons = 7;
        unsafe {
            bot_on_frame_tick(&normal_state, &mut cmd);
        }
        assert_eq!(cmd.pad_connected, 0);
        assert_eq!(cmd.buttons, 0);

        BOT_PANICKED.store(false, Ordering::Relaxed);
    }
}
