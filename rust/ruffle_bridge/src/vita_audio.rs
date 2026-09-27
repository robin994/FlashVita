use crate::vita_navigator::log_line;
use ruffle_core::backend::audio::{
    AudioBackend, AudioMixer, AudioMixerProxy, DecodeError, RegisterError, SoundHandle,
    SoundInstanceHandle, SoundStreamInfo, SoundTransform, swf,
};
use ruffle_core::impl_audio_mixer_backend;
use std::ffi::c_void;

const OUTPUT_CHANNELS: u8 = 2;
const OUTPUT_SAMPLE_RATE: u32 = 48_000;

type AudioFillCallback = unsafe extern "C" fn(*mut c_void, *mut i16, u32);

unsafe extern "C" {
    fn flashvita_vita_audio_create(
        callback: AudioFillCallback,
        user: *mut c_void,
    ) -> *mut c_void;
    fn flashvita_vita_audio_set_paused(handle: *mut c_void, paused: i32) -> i32;
    fn flashvita_vita_audio_get_stats(
        handle: *mut c_void,
        buffers: *mut u64,
        last_error: *mut i32,
        run_clocks: *mut u64,
        last_cpu: *mut i32,
    ) -> i32;
    fn flashvita_vita_audio_destroy(handle: *mut c_void);
}

struct AudioThreadState {
    mixer: AudioMixerProxy,
}

unsafe extern "C" fn fill_audio(user: *mut c_void, samples: *mut i16, frames: u32) {
    if user.is_null() || samples.is_null() || frames == 0 {
        return;
    }

    let state = &*(user.cast::<AudioThreadState>());
    let sample_count = frames as usize * OUTPUT_CHANNELS as usize;
    let output = std::slice::from_raw_parts_mut(samples, sample_count);
    state.mixer.mix::<i16>(output);
}

pub struct VitaAudioBackend {
    mixer: AudioMixer,
    native_handle: *mut c_void,
    thread_state: *mut AudioThreadState,
}

impl VitaAudioBackend {
    pub fn new() -> Option<Self> {
        let mixer = AudioMixer::new(OUTPUT_CHANNELS, OUTPUT_SAMPLE_RATE);
        let thread_state = Box::into_raw(Box::new(AudioThreadState {
            mixer: mixer.proxy(),
        }));
        let native_handle = unsafe {
            flashvita_vita_audio_create(fill_audio, thread_state.cast::<c_void>())
        };

        if native_handle.is_null() {
            unsafe {
                drop(Box::from_raw(thread_state));
            }
            log_line("audio_backend init_failed -> NullAudioBackend fallback");
            return None;
        }

        log_line("audio_backend vita_native mixer=stereo/48000 thread=dedicated");
        Some(Self {
            mixer,
            native_handle,
            thread_state,
        })
    }
}

impl AudioBackend for VitaAudioBackend {
    impl_audio_mixer_backend!(mixer);

    fn play(&mut self) {
        unsafe {
            flashvita_vita_audio_set_paused(self.native_handle, 0);
        }
    }

    fn pause(&mut self) {
        unsafe {
            flashvita_vita_audio_set_paused(self.native_handle, 1);
        }
    }
}

impl Drop for VitaAudioBackend {
    fn drop(&mut self) {
        let mut buffers = 0u64;
        let mut last_error = 0i32;
        let mut run_clocks = 0u64;
        let mut last_cpu = -1i32;
        unsafe {
            flashvita_vita_audio_get_stats(
                self.native_handle,
                &mut buffers,
                &mut last_error,
                &mut run_clocks,
                &mut last_cpu,
            );
        }
        log_line(&format!(
            "audio_backend stop buffers={} last_error=0x{:08X} run_clocks={} last_cpu={}",
            buffers, last_error as u32, run_clocks, last_cpu
        ));

        unsafe {
            flashvita_vita_audio_destroy(self.native_handle);
            drop(Box::from_raw(self.thread_state));
        }
        self.native_handle = std::ptr::null_mut();
        self.thread_state = std::ptr::null_mut();
    }
}
