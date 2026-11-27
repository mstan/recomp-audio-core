// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthew Stanley
// Include with `native` bound to pcm_session.rs. Exactly one producer/consumer.
pub use native::{MAX_SAMPLES, PACKETS};
use std::ffi::{c_int, c_void};
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::ptr::{NonNull, null_mut};
use std::sync::Arc;

pub trait PacketObserver: Send + Sync + 'static {
    fn now_ns() -> u64;
    fn produced(samples: usize, field: u64, produced_ns: u64) -> u64;
    fn send_returned(sequence: u64);
    fn discarded(sequence: u64, reason: &'static str);
    fn submitted(sequence: u64, submission: &native::Submission);
    fn first_paced_overflow() {}
}
pub trait PacketSink {
    /// # Safety
    /// Session remains alive and consumer-exclusive throughout this call.
    unsafe fn fill_native(
        &mut self,
        session: *mut native::PacketSession,
        requested: usize,
        started: u64,
    );
}
struct Handle<O: PacketObserver> {
    ptr: NonNull<native::PacketSession>,
    _observer: PhantomData<O>,
}
// The native queue publishes complete slots using release/acquire indices.
// Endpoint methods require &mut, and only atomic stats are shared separately.
unsafe impl<O: PacketObserver> Send for Handle<O> {}
unsafe impl<O: PacketObserver> Sync for Handle<O> {}
impl<O: PacketObserver> Drop for Handle<O> {
    fn drop(&mut self) {
        unsafe { native::recomp_audio_packet_destroy(self.ptr.as_ptr()) };
    }
}
pub struct Producer<O: PacketObserver> {
    queue: Arc<Handle<O>>,
}
pub struct Consumer<O: PacketObserver> {
    queue: Arc<Handle<O>>,
}
pub struct Stats<O: PacketObserver> {
    queue: Arc<Handle<O>>,
}
impl<O: PacketObserver> Stats<O> {
    pub fn take_submit_ns(&self) -> u64 {
        unsafe { native::recomp_audio_packet_take_submit_ns(self.queue.ptr.as_ptr()) }
    }
}
unsafe extern "C" fn clock<O: PacketObserver>(_: *mut c_void) -> u64 {
    O::now_ns()
}
unsafe extern "C" fn produced<O: PacketObserver>(
    _: *mut c_void,
    samples: usize,
    field: u64,
    at: u64,
) -> u64 {
    O::produced(samples, field, at)
}
unsafe extern "C" fn returned<O: PacketObserver>(_: *mut c_void, sequence: u64) {
    O::send_returned(sequence);
}
unsafe extern "C" fn discarded<O: PacketObserver>(
    _: *mut c_void,
    sequence: u64,
    reason: c_int,
    first: c_int,
) {
    if first != 0 {
        O::first_paced_overflow();
    }
    let reason = match reason {
        native::PACED_OVERFLOW => "paced_overflow",
        native::UNPACED_OVERFLOW => "unpaced_overflow",
        native::PACED_BACKLOG => "paced_backlog",
        native::CLEARED => "cleared",
        _ => "closed",
    };
    O::discarded(sequence, reason);
}
unsafe extern "C" fn submitted<O: PacketObserver>(
    _: *mut c_void,
    sequence: u64,
    s: *const native::Submission,
) {
    O::submitted(sequence, unsafe { &*s });
}
unsafe extern "C" fn discard_reason<O: PacketObserver>(user: *mut c_void, sequence: u64) {
    // Native discard_notify invokes synchronously and never retains this pointer.
    let reason = unsafe { *user.cast::<&'static str>() };
    O::discarded(sequence, reason);
}
pub fn channel<O: PacketObserver>(
    rate: u32,
    channels: u32,
    realtime: bool,
) -> (Producer<O>, Consumer<O>) {
    assert!(rate > 0 && (1..=2).contains(&channels));
    let observer = native::Observer {
        user: null_mut(),
        clock_ns: Some(clock::<O>),
        produced: Some(produced::<O>),
        send_returned: Some(returned::<O>),
        discarded: Some(discarded::<O>),
        submitted: Some(submitted::<O>),
    };
    let ptr = NonNull::new(unsafe {
        native::recomp_audio_packet_create(rate, channels, realtime as c_int, &observer)
    })
    .expect("Could not allocate bounded PCM session");
    let queue = Arc::new(Handle {
        ptr,
        _observer: PhantomData,
    });
    (
        Producer {
            queue: queue.clone(),
        },
        Consumer { queue },
    )
}
impl<O: PacketObserver> Producer<O> {
    pub fn submit(&mut self, samples: &[i16], field: u64) -> Result<(), &'static str> {
        self.submit_at(samples, field, O::now_ns())
    }
    pub fn submit_at(&mut self, samples: &[i16], field: u64, at: u64) -> Result<(), &'static str> {
        if unsafe {
            native::recomp_audio_packet_submit(
                self.queue.ptr.as_ptr(),
                samples.as_ptr(),
                samples.len(),
                field,
                at,
            )
        } < 0
        {
            Err("PCM field exceeded the handoff bound or split an interleaved frame")
        } else {
            Ok(())
        }
    }
    pub fn queued_frames(&self) -> u32 {
        unsafe { native::recomp_audio_packet_queued_frames(self.queue.ptr.as_ptr()) }
    }
}
pub struct PacketView<'a> {
    pub sequence: u64,
    pub produced_ns: u64,
    pub len: usize,
    pub samples: &'a [i16],
}
impl<O: PacketObserver> Consumer<O> {
    pub fn stats(&self) -> Stats<O> {
        Stats {
            queue: self.queue.clone(),
        }
    }
    pub fn read_position(&self) -> usize {
        unsafe { native::recomp_audio_packet_read_position(self.queue.ptr.as_ptr()) }
    }
    pub fn priming(&self) -> bool {
        unsafe { native::recomp_audio_packet_priming(self.queue.ptr.as_ptr()) != 0 }
    }
    pub fn consume(&mut self, use_packet: impl for<'a> FnOnce(&'a PacketView<'a>)) -> bool {
        let mut packet = MaybeUninit::<native::Packet>::uninit();
        if unsafe { native::recomp_audio_packet_peek(self.queue.ptr.as_ptr(), packet.as_mut_ptr()) }
            == 0
        {
            return false;
        }
        let packet = unsafe { packet.assume_init() };
        use_packet(&PacketView {
            sequence: packet.sequence,
            produced_ns: packet.produced_ns,
            len: packet.samples,
            samples: unsafe { std::slice::from_raw_parts(packet.pcm, packet.samples) },
        });
        unsafe { native::recomp_audio_packet_advance(self.queue.ptr.as_ptr()) };
        true
    }
    pub fn ready_for_pull(&mut self, requested: usize, now: u64) -> bool {
        unsafe { native::recomp_audio_packet_ready(self.queue.ptr.as_ptr(), requested, now) != 0 }
    }
    pub fn prune_backlog(&mut self, now: u64, queued_frames: u32) {
        unsafe { native::recomp_audio_packet_prune(self.queue.ptr.as_ptr(), now, queued_frames) };
    }
    pub fn fill_at(&mut self, sink: &mut impl PacketSink, requested: usize, started: u64) {
        // The retained handle and unique consumer borrow satisfy the sink contract.
        unsafe { sink.fill_native(self.queue.ptr.as_ptr(), requested, started) };
    }
    pub fn discard(&mut self, reason: &'static str) {
        unsafe {
            native::recomp_audio_packet_discard_notify(
                self.queue.ptr.as_ptr(),
                (&reason as *const &'static str).cast_mut().cast(),
                Some(discard_reason::<O>),
            )
        };
    }
}
