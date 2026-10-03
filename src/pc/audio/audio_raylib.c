#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>

#include "raylib.h"
#include "audio_api.h"

// SM64 produces 32000 Hz, 16-bit, stereo PCM.
#define SAMPLE_RATE   32000
#define CHANNELS      2

// Frames handed to the audio thread per callback (raylib may round this up
// to the device period; the callback is told the real count either way).
#define STREAM_CHUNK  512

// Ring buffer between the game thread (producer) and raylib's audio thread
// (consumer). Must be a power of two. Counters only ever increase.
#define RING_FRAMES   8192

// Engine tries to keep about this many frames buffered. Lower = less latency,
// higher = safer against crackling. 32000 frames = 1 second.
#define DESIRED_BUFFERED 1400

static AudioStream stream;
static bool stream_ready = false;
static bool device_opened_by_us = false;

static int16_t ring[RING_FRAMES * CHANNELS];
static atomic_size_t ring_write = 0; // frames written (game thread)
static atomic_size_t ring_read = 0;  // frames consumed (audio thread)

// Runs on raylib's audio thread. Always fills the whole buffer: any frames
// we don't have are silence, which beats raylib replaying stale data.
static void stream_callback(void *buffer_data, unsigned int frames) {
    int16_t *out = (int16_t *)buffer_data;

    size_t r = atomic_load_explicit(&ring_read, memory_order_relaxed);
    size_t w = atomic_load_explicit(&ring_write, memory_order_acquire);
    size_t avail = w - r;
    size_t take = avail < frames ? avail : frames;

    for (size_t i = 0; i < take; i++) {
        size_t idx = ((r + i) & (RING_FRAMES - 1)) * CHANNELS;
        out[i * CHANNELS + 0] = ring[idx + 0];
        out[i * CHANNELS + 1] = ring[idx + 1];
    }
    if (take < frames) {
        memset(out + take * CHANNELS, 0, (frames - take) * CHANNELS * sizeof(int16_t));
    }

    atomic_store_explicit(&ring_read, r + take, memory_order_release);
}

static bool audio_raylib_init(void) {
    if (!IsAudioDeviceReady()) {
        InitAudioDevice();
        device_opened_by_us = true;
    }
    if (!IsAudioDeviceReady()) {
        return false;
    }

    // Must be set before LoadAudioStream.
    SetAudioStreamBufferSizeDefault(STREAM_CHUNK);
    stream = LoadAudioStream(SAMPLE_RATE, 16, CHANNELS);
    SetAudioStreamCallback(stream, stream_callback);
    PlayAudioStream(stream);
    stream_ready = true;

    return true;
}

// Frames currently queued and not yet handed to the sound card.
static int audio_raylib_buffered(void) {
    size_t w = atomic_load_explicit(&ring_write, memory_order_relaxed);
    size_t r = atomic_load_explicit(&ring_read, memory_order_acquire);
    return (int)(w - r);
}

static int audio_raylib_get_desired_buffered(void) {
    return DESIRED_BUFFERED;
}

// len is in bytes: 16-bit stereo = 4 bytes per frame.
static void audio_raylib_play(const uint8_t *buf, size_t len) {
    if (!stream_ready) return;

    size_t frames = len / (sizeof(int16_t) * CHANNELS);
    const int16_t *in = (const int16_t *)buf;

    size_t w = atomic_load_explicit(&ring_write, memory_order_relaxed);
    size_t r = atomic_load_explicit(&ring_read, memory_order_acquire);
    size_t free_frames = RING_FRAMES - (w - r);

    // If the game runs far ahead of the card, drop the excess instead of
    // growing latency without bound.
    if (frames > free_frames) frames = free_frames;

    for (size_t i = 0; i < frames; i++) {
        size_t idx = ((w + i) & (RING_FRAMES - 1)) * CHANNELS;
        ring[idx + 0] = in[i * CHANNELS + 0];
        ring[idx + 1] = in[i * CHANNELS + 1];
    }

    atomic_store_explicit(&ring_write, w + frames, memory_order_release);
}

struct AudioAPI audio_wasapi = {
    audio_raylib_init,
    audio_raylib_buffered,
    audio_raylib_get_desired_buffered,
    audio_raylib_play
};