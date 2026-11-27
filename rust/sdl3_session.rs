// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley
// Include with `native` bound to pcm_session.rs and SDL3 as a crate dependency.
// Session/queue policy and lifetime live in the native provider, below these
// direct SDL system-call bindings. The context owns subsystem/gain policy.
use sdl3::sys::audio::{
    SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, SDL_AUDIO_FRAMESIZE, SDL_AUDIO_S16, SDL_AudioSpec,
    SDL_AudioStream, SDL_ClearAudioStream, SDL_DestroyAudioStream, SDL_GetAudioStreamAvailable,
    SDL_GetAudioStreamFormat, SDL_GetAudioStreamFrequencyRatio, SDL_GetAudioStreamQueued,
    SDL_GetCurrentAudioDriver, SDL_LockAudioStream, SDL_OpenAudioDeviceStream,
    SDL_PutAudioStreamData, SDL_ResumeAudioStreamDevice, SDL_SetAudioStreamGain,
    SDL_SetAudioStreamGetCallback, SDL_UnlockAudioStream,
};
use std::ffi::{CStr, c_int, c_void};
use std::marker::PhantomData;
use std::ptr::{NonNull, null_mut};

pub trait AudioDeviceContext: Sized {
    fn acquire() -> Result<Self, String>;
    fn gain() -> f32;
    fn opened(_driver: &CStr) {}
}
pub trait PcmSource: Send + 'static {
    fn fill(&mut self, stream: &mut PcmStream, requested_samples: usize);
    fn observe_pull(&mut self, _pull: PcmPull) {}
    fn discard(&mut self, reason: &'static str);
}
#[derive(Debug, Default)]
pub struct PcmPull {
    pub source_hz: Option<i32>,
    pub destination_hz: Option<i32>,
    pub requested_frames: Option<u32>,
    pub available_frames: Option<u32>,
    pub unobserved: Option<&'static str>,
}
impl From<native::Pull> for PcmPull {
    fn from(p: native::Pull) -> Self {
        Self {
            source_hz: (p.source_hz > 0).then_some(p.source_hz),
            destination_hz: (p.destination_hz > 0).then_some(p.destination_hz),
            requested_frames: (p.reason == 0).then_some(p.requested_frames),
            available_frames: (p.reason == 0).then_some(p.available_frames),
            unobserved: match p.reason {
                0 => None,
                1 => Some("stream_format_unavailable"),
                2 => Some("resampling"),
                3 => Some("prior_rate_unobserved"),
                4 => Some("invalid_request"),
                5 => Some("availability_unavailable"),
                6 => Some("invalid_availability"),
                _ => Some("unknown_observation"),
            },
        }
    }
}
pub fn pull_frames(
    total: c_int,
    source_frame_bytes: u32,
    available: Option<u32>,
    destination_frame_bytes: u32,
) -> Result<(u32, u32), &'static str> {
    let info = native::StreamInfo {
        source_hz: 1,
        destination_hz: 1,
        source_frame_bytes,
        destination_frame_bytes,
        available_bytes: available.map_or(-1, i64::from),
        frequency_ratio: 1.,
        format_available: 1,
        ..Default::default()
    };
    let mut unit = 1;
    let p = unsafe { native::recomp_audio_observe_pull(&info, total, &mut unit) };
    if p.reason == 0 {
        Ok((p.requested_frames, p.available_frames))
    } else {
        Err(PcmPull::from(p).unobserved.unwrap_or("unknown_observation"))
    }
}
unsafe extern "C" fn stream_info(
    _: *mut c_void,
    stream: *mut c_void,
    fields: u32,
) -> native::StreamInfo {
    let stream = stream.cast::<SDL_AudioStream>();
    let mut info = native::StreamInfo {
        queued_bytes: -1,
        available_bytes: -1,
        ..Default::default()
    };
    if fields & native::QUERY_QUEUED != 0 {
        info.queued_bytes = i64::from(unsafe { SDL_GetAudioStreamQueued(stream) });
    }
    if fields & native::QUERY_AVAILABLE != 0 {
        info.available_bytes = i64::from(unsafe { SDL_GetAudioStreamAvailable(stream) });
    }
    if fields & native::QUERY_FORMAT != 0 {
        let mut source = SDL_AudioSpec {
            format: SDL_AUDIO_S16,
            channels: 0,
            freq: 0,
        };
        let mut destination = source;
        if unsafe { SDL_GetAudioStreamFormat(stream, &mut source, &mut destination) }
            && source.freq > 0
            && destination.freq > 0
            && source.channels > 0
            && destination.channels > 0
        {
            info.format_available = 1;
            info.source_hz = source.freq;
            info.destination_hz = destination.freq;
            info.source_frame_bytes = SDL_AUDIO_FRAMESIZE(source);
            info.destination_frame_bytes = SDL_AUDIO_FRAMESIZE(destination);
            info.frequency_ratio = unsafe { SDL_GetAudioStreamFrequencyRatio(stream) };
        }
    }
    info
}
unsafe extern "C" fn put(
    _: *mut c_void,
    stream: *mut c_void,
    pcm: *const i16,
    samples: usize,
) -> c_int {
    let Some(bytes) = samples
        .checked_mul(std::mem::size_of::<i16>())
        .and_then(|n| c_int::try_from(n).ok())
    else {
        return 0;
    };
    i32::from(unsafe { SDL_PutAudioStreamData(stream.cast(), pcm.cast(), bytes) })
}
fn sink() -> native::Sink {
    native::Sink {
        user: null_mut(),
        info: Some(stream_info),
        put: Some(put),
    }
}
/// Borrowed only during the serialized stream callback.
pub struct PcmStream(NonNull<SDL_AudioStream>);
impl PcmStream {
    pub fn queued_bytes(&self) -> Option<u32> {
        u32::try_from(
            unsafe { stream_info(null_mut(), self.0.as_ptr().cast(), native::QUERY_QUEUED) }
                .queued_bytes,
        )
        .ok()
    }
    pub fn available_bytes(&self) -> Option<u32> {
        u32::try_from(
            unsafe { stream_info(null_mut(), self.0.as_ptr().cast(), native::QUERY_AVAILABLE) }
                .available_bytes,
        )
        .ok()
    }
    pub fn put_samples(&mut self, samples: &[i16]) -> bool {
        unsafe {
            put(
                null_mut(),
                self.0.as_ptr().cast(),
                samples.as_ptr(),
                samples.len(),
            ) != 0
        }
    }
    /// The session must remain live and have exactly one serialized consumer.
    pub unsafe fn fill_packet_session(
        &mut self,
        session: *mut native::PacketSession,
        requested: usize,
        started: u64,
    ) {
        unsafe {
            native::recomp_audio_packet_fill(
                session,
                &sink(),
                self.0.as_ptr().cast(),
                requested,
                started,
            )
        };
    }
}
unsafe extern "C" fn open(
    _: *mut c_void,
    rate: u32,
    channels: u32,
    callback: native::PullCallback,
    user: *mut c_void,
) -> *mut c_void {
    let spec = SDL_AudioSpec {
        format: SDL_AUDIO_S16,
        channels: channels as i32,
        freq: rate as i32,
    };
    // The C ABI arguments differ only in the spelling of the opaque pointer.
    let callback = unsafe {
        std::mem::transmute::<
            native::PullCallback,
            unsafe extern "C" fn(*mut c_void, *mut SDL_AudioStream, c_int, c_int),
        >(callback)
    };
    unsafe {
        SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
            &spec,
            Some(callback),
            user,
        )
    }
    .cast()
}
unsafe extern "C" fn resume(_: *mut c_void, s: *mut c_void) -> c_int {
    i32::from(unsafe { SDL_ResumeAudioStreamDevice(s.cast()) })
}
unsafe extern "C" fn gain(_: *mut c_void, s: *mut c_void, value: f32) -> c_int {
    i32::from(unsafe { SDL_SetAudioStreamGain(s.cast(), value) })
}
unsafe extern "C" fn current_gain<C: AudioDeviceContext>(_: *mut c_void) -> f32 {
    C::gain()
}
unsafe extern "C" fn lock(_: *mut c_void, s: *mut c_void) -> c_int {
    i32::from(unsafe { SDL_LockAudioStream(s.cast()) })
}
unsafe extern "C" fn unlock(_: *mut c_void, s: *mut c_void) {
    unsafe { SDL_UnlockAudioStream(s.cast()) };
}
unsafe extern "C" fn clear(_: *mut c_void, s: *mut c_void) -> c_int {
    i32::from(unsafe { SDL_ClearAudioStream(s.cast()) })
}
unsafe extern "C" fn detach(_: *mut c_void, s: *mut c_void) {
    unsafe { SDL_SetAudioStreamGetCallback(s.cast(), None, null_mut()) };
}
unsafe extern "C" fn destroy(_: *mut c_void, s: *mut c_void) {
    unsafe { SDL_DestroyAudioStream(s.cast()) };
}
unsafe extern "C" fn source_fill<S: PcmSource>(
    p: *mut c_void,
    stream: *mut c_void,
    samples: usize,
) {
    let Some(stream) = NonNull::new(stream.cast()) else {
        return;
    };
    unsafe { &mut *p.cast::<S>() }.fill(&mut PcmStream(stream), samples);
}
unsafe extern "C" fn source_observe<S: PcmSource>(p: *mut c_void, pull: *const native::Pull) {
    unsafe { &mut *p.cast::<S>() }.observe_pull(unsafe { *pull }.into());
}
unsafe extern "C" fn source_discard<S: PcmSource>(p: *mut c_void, reason: c_int) {
    unsafe { &mut *p.cast::<S>() }.discard(if reason == native::CLEARED {
        "cleared"
    } else {
        "closed"
    });
}
unsafe extern "C" fn source_destroy<S: PcmSource>(p: *mut c_void) {
    drop(unsafe { Box::from_raw(p.cast::<S>()) });
}
pub struct Sdl3PullAudio<S: PcmSource, C: AudioDeviceContext> {
    session: NonNull<native::OutputSession>,
    _context: C,
    _source: PhantomData<S>,
}
impl<S: PcmSource, C: AudioDeviceContext> Sdl3PullAudio<S, C> {
    pub fn new() -> Result<Self, String> {
        let context = C::acquire()?;
        let backend = native::Backend {
            sink: sink(),
            open: Some(open),
            resume: Some(resume),
            set_gain: Some(gain),
            gain: Some(current_gain::<C>),
            lock: Some(lock),
            unlock: Some(unlock),
            clear: Some(clear),
            detach: Some(detach),
            destroy: Some(destroy),
        };
        let session = NonNull::new(unsafe { native::recomp_audio_output_create(&backend) })
            .ok_or("Could not allocate PCM output session")?;
        Ok(Self {
            session,
            _context: context,
            _source: PhantomData,
        })
    }
    pub fn open(&mut self, rate: u32, channels: u32, source: S) -> bool {
        let source = native::Source {
            user: Box::into_raw(Box::new(source)).cast(),
            fill: Some(source_fill::<S>),
            observe_pull: Some(source_observe::<S>),
            discard: Some(source_discard::<S>),
            destroy: Some(source_destroy::<S>),
        };
        if unsafe {
            native::recomp_audio_output_open(self.session.as_ptr(), rate, channels, source)
        } == 0
        {
            return false;
        }
        let driver = unsafe { SDL_GetCurrentAudioDriver() };
        if !driver.is_null() {
            C::opened(unsafe { CStr::from_ptr(driver) });
        }
        true
    }
    pub fn clear(&mut self) -> Result<(), String> {
        if unsafe { native::recomp_audio_output_clear(self.session.as_ptr()) } != 0 {
            Ok(())
        } else {
            Err(sdl3::get_error().to_string())
        }
    }
    /// Owner-thread borrowed stream, invalidated by successful reopen or drop.
    pub fn raw_stream(&self) -> *mut SDL_AudioStream {
        unsafe { native::recomp_audio_output_stream(self.session.as_ptr()) }.cast()
    }
}
impl<S: PcmSource, C: AudioDeviceContext> Drop for Sdl3PullAudio<S, C> {
    fn drop(&mut self) {
        unsafe { native::recomp_audio_output_destroy(self.session.as_ptr()) };
    }
}
