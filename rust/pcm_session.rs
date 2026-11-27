// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley
// C ABI for native PCM ownership. Device APIs stay in the host binding.
use std::ffi::{c_int, c_void};
#[repr(C)]
pub struct PacketSession {
    _private: [u8; 0],
}
#[repr(C)]
pub struct OutputSession {
    _private: [u8; 0],
}
pub const PACKETS: usize = 16;
pub const MAX_SAMPLES: usize = 192_000;
pub const PACED_OVERFLOW: c_int = 1;
pub const UNPACED_OVERFLOW: c_int = 2;
pub const PACED_BACKLOG: c_int = 3;
pub const CLEARED: c_int = 4;
pub const CLOSED: c_int = 5;
pub const QUERY_QUEUED: u32 = 1;
pub const QUERY_AVAILABLE: u32 = 2;
pub const QUERY_FORMAT: u32 = 4;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Packet {
    pub sequence: u64,
    pub produced_ns: u64,
    pub samples: usize,
    pub pcm: *const i16,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct StreamInfo {
    pub queued_bytes: i64,
    pub available_bytes: i64,
    pub source_hz: i32,
    pub destination_hz: i32,
    pub source_frame_bytes: u32,
    pub destination_frame_bytes: u32,
    pub frequency_ratio: f32,
    pub format_available: c_int,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Sink {
    pub user: *mut c_void,
    pub info: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, u32) -> StreamInfo>,
    pub put: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, *const i16, usize) -> c_int>,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Submission {
    pub dequeued_ns: u64,
    pub put_started_ns: u64,
    pub put_completed_ns: u64,
    pub queued_before_frames: u32,
    pub queued_after_frames: u32,
    pub available_before_bytes: u32,
    pub available_after_bytes: u32,
    pub valid: u32,
    pub succeeded: c_int,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Observer {
    pub user: *mut c_void,
    pub clock_ns: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub produced: Option<unsafe extern "C" fn(*mut c_void, usize, u64, u64) -> u64>,
    pub send_returned: Option<unsafe extern "C" fn(*mut c_void, u64)>,
    pub discarded: Option<unsafe extern "C" fn(*mut c_void, u64, c_int, c_int)>,
    pub submitted: Option<unsafe extern "C" fn(*mut c_void, u64, *const Submission)>,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Pull {
    pub source_hz: i32,
    pub destination_hz: i32,
    pub requested_frames: u32,
    pub available_frames: u32,
    pub reason: c_int,
}
pub type PullCallback = unsafe extern "C" fn(*mut c_void, *mut c_void, i32, i32);
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Backend {
    pub sink: Sink,
    pub open: Option<
        unsafe extern "C" fn(*mut c_void, u32, u32, PullCallback, *mut c_void) -> *mut c_void,
    >,
    pub resume: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> c_int>,
    pub set_gain: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, f32) -> c_int>,
    pub gain: Option<unsafe extern "C" fn(*mut c_void) -> f32>,
    pub lock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> c_int>,
    pub unlock: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub clear: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> c_int>,
    pub detach: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Source {
    pub user: *mut c_void,
    pub fill: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, usize)>,
    pub observe_pull: Option<unsafe extern "C" fn(*mut c_void, *const Pull)>,
    pub discard: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}
unsafe extern "C" {
    pub fn recomp_audio_session_clock_ns() -> u64;
    pub fn recomp_audio_packet_create(
        rate: u32,
        channels: u32,
        paced: c_int,
        observer: *const Observer,
    ) -> *mut PacketSession;
    pub fn recomp_audio_packet_destroy(session: *mut PacketSession);
    pub fn recomp_audio_packet_submit(
        session: *mut PacketSession,
        pcm: *const i16,
        samples: usize,
        field: u64,
        produced_ns: u64,
    ) -> c_int;
    pub fn recomp_audio_packet_peek(session: *mut PacketSession, packet: *mut Packet) -> c_int;
    pub fn recomp_audio_packet_advance(session: *mut PacketSession);
    pub fn recomp_audio_packet_prune(session: *mut PacketSession, now_ns: u64, device_frames: u32);
    pub fn recomp_audio_packet_ready(
        session: *mut PacketSession,
        requested: usize,
        now_ns: u64,
    ) -> c_int;
    pub fn recomp_audio_packet_fill(
        session: *mut PacketSession,
        sink: *const Sink,
        stream: *mut c_void,
        requested: usize,
        started_ns: u64,
    );
    pub fn recomp_audio_packet_discard(session: *mut PacketSession, reason: c_int);
    pub fn recomp_audio_packet_discard_notify(
        session: *mut PacketSession,
        user: *mut c_void,
        discarded: Option<unsafe extern "C" fn(*mut c_void, u64)>,
    );
    pub fn recomp_audio_packet_queued_frames(session: *const PacketSession) -> u32;
    pub fn recomp_audio_packet_take_submit_ns(session: *mut PacketSession) -> u64;
    pub fn recomp_audio_packet_read_position(session: *const PacketSession) -> usize;
    pub fn recomp_audio_packet_priming(session: *const PacketSession) -> c_int;
    pub fn recomp_audio_output_create(backend: *const Backend) -> *mut OutputSession;
    pub fn recomp_audio_output_destroy(session: *mut OutputSession);
    pub fn recomp_audio_output_open(
        session: *mut OutputSession,
        rate: u32,
        channels: u32,
        source: Source,
    ) -> c_int;
    pub fn recomp_audio_output_clear(session: *mut OutputSession) -> c_int;
    pub fn recomp_audio_output_stream(session: *const OutputSession) -> *mut c_void;
    pub fn recomp_audio_observe_pull(
        info: *const StreamInfo,
        total: i32,
        unit_rate_only: *mut c_int,
    ) -> Pull;
}
