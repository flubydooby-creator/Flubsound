// ABI checks for platform/windows/driver/FlubVirtualAudioShared.h, the header
// shared by the Windows WaveRT driver (C, kernel mode) and the engine (C++,
// user mode). Outside _KERNEL_MODE it needs only <stdint.h> / <stddef.h>, so
// it is checked on every platform:
//   * compile time: the control block is padding-free, every field sits in
//     its documented cache line, 64-bit fields are 8-byte aligned, the IOCTL
//     structs are 32/64-bit neutral and the IOCTL codes decode as CTL_CODE;
//   * run time: the magic's byte order, the ring index maths of the
//     memory-ordering contract (BufferFrames need not be a power of two, frame
//     counters are 64-bit) and the Generation sequence lock;
//   * gcc / clang builds also compile the header as strict C89
//     (test_driver_shared_c.c) and compare the C layout with this one.
#include "TestFramework.h"

#include "FlubVirtualAudioShared.h"

#include "flub/common/AudioBlock.h" // flub::kMaxChannels

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

using namespace flubtest;

namespace
{
using Control = FLUB_VA_STREAM_CONTROL;

constexpr bool isPowerOfTwo (unsigned long long v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

/** Field [offset, offset + size) lies inside cache line `line`. */
constexpr bool inCacheLine (size_t offset, size_t size, unsigned line) noexcept
{
    return offset >= line * FLUB_VA_CACHE_LINE_BYTES && offset + size <= (line + 1) * FLUB_VA_CACHE_LINE_BYTES;
}

// ---- types -------------------------------------------------------------------
static_assert (sizeof (FLUB_VA_U8) == 1 && sizeof (FLUB_VA_U32) == 4 && sizeof (FLUB_VA_U64) == 8 && sizeof (FLUB_VA_I64) == 8);
static_assert (std::is_unsigned_v<FLUB_VA_U64> && std::is_signed_v<FLUB_VA_I64>);
static_assert (std::is_standard_layout_v<Control>, "offsetof must be meaningful");
static_assert (isPowerOfTwo (FLUB_VA_CACHE_LINE_BYTES) && isPowerOfTwo (FLUB_VA_PAGE_BYTES));
static_assert (FLUB_VA_PAGE_BYTES % FLUB_VA_CACHE_LINE_BYTES == 0, "a page-aligned control block keeps cache-line alignment");

// ---- control block: no implicit padding anywhere --------------------------------
// Each field starts where the previous one ends, so the kernel (C) and the
// engine (C++, any compiler) cannot disagree about hidden padding.
#define FLUB_VA_FOLLOWS(prev, next) (offsetof (Control, next) == offsetof (Control, prev) + sizeof (Control::prev))
static_assert (offsetof (Control, Magic) == 0);
static_assert (FLUB_VA_FOLLOWS (Magic, AbiVersion) && FLUB_VA_FOLLOWS (AbiVersion, StructBytes)
               && FLUB_VA_FOLLOWS (StructBytes, EndpointIndex) && FLUB_VA_FOLLOWS (EndpointIndex, Direction)
               && FLUB_VA_FOLLOWS (Direction, SampleFormat) && FLUB_VA_FOLLOWS (SampleFormat, SampleRate)
               && FLUB_VA_FOLLOWS (SampleRate, NumChannels) && FLUB_VA_FOLLOWS (NumChannels, ChannelMask)
               && FLUB_VA_FOLLOWS (ChannelMask, BytesPerFrame) && FLUB_VA_FOLLOWS (BytesPerFrame, BufferFrames)
               && FLUB_VA_FOLLOWS (BufferFrames, PeriodFrames) && FLUB_VA_FOLLOWS (PeriodFrames, ClockMode)
               && FLUB_VA_FOLLOWS (ClockMode, Generation) && FLUB_VA_FOLLOWS (Generation, DataCapacityBytes)
               && FLUB_VA_FOLLOWS (DataCapacityBytes, WritePosition),
               "cache line 0 (description) is packed and ends at line 1");
static_assert (FLUB_VA_FOLLOWS (WritePosition, WriteQpc) && FLUB_VA_FOLLOWS (WriteQpc, Overruns)
               && FLUB_VA_FOLLOWS (Overruns, ProducerFlags) && FLUB_VA_FOLLOWS (ProducerFlags, Reserved1)
               && FLUB_VA_FOLLOWS (Reserved1, ReadPosition),
               "cache line 1 (producer) is packed and ends at line 2");
static_assert (FLUB_VA_FOLLOWS (ReadPosition, ReadQpc) && FLUB_VA_FOLLOWS (ReadQpc, Underruns)
               && FLUB_VA_FOLLOWS (Underruns, ConsumerFlags) && FLUB_VA_FOLLOWS (ConsumerFlags, ReadOffsetBytes)
               && FLUB_VA_FOLLOWS (ReadOffsetBytes, Reserved2) && FLUB_VA_FOLLOWS (Reserved2, StreamState),
               "cache line 2 (consumer) is packed and ends at line 3");
static_assert (FLUB_VA_FOLLOWS (StreamState, EngineAttached) && FLUB_VA_FOLLOWS (EngineAttached, EngineHeartbeat)
               && FLUB_VA_FOLLOWS (EngineHeartbeat, RequestedClockMode) && FLUB_VA_FOLLOWS (RequestedClockMode, QpcFrequency)
               && FLUB_VA_FOLLOWS (QpcFrequency, Reserved3),
               "cache line 3 (state) is packed");
static_assert (offsetof (Control, Reserved3) + sizeof (Control::Reserved3) == sizeof (Control), "no tail padding");
#undef FLUB_VA_FOLLOWS

// ---- control block: cache-line ownership and 64-bit alignment --------------------
#define FLUB_VA_IN_LINE(field, line) inCacheLine (offsetof (Control, field), sizeof (Control::field), line)
static_assert (FLUB_VA_IN_LINE (Generation, 0) && FLUB_VA_IN_LINE (DataCapacityBytes, 0), "description line");
static_assert (FLUB_VA_IN_LINE (WritePosition, 1) && FLUB_VA_IN_LINE (WriteQpc, 1) && FLUB_VA_IN_LINE (Overruns, 1)
                   && FLUB_VA_IN_LINE (ProducerFlags, 1),
               "producer-owned fields share no line with the consumer");
static_assert (FLUB_VA_IN_LINE (ReadPosition, 2) && FLUB_VA_IN_LINE (ReadQpc, 2) && FLUB_VA_IN_LINE (Underruns, 2)
                   && FLUB_VA_IN_LINE (ConsumerFlags, 2) && FLUB_VA_IN_LINE (ReadOffsetBytes, 2),
               "consumer-owned fields share no line with the producer");
static_assert (FLUB_VA_IN_LINE (StreamState, 3) && FLUB_VA_IN_LINE (EngineHeartbeat, 3) && FLUB_VA_IN_LINE (QpcFrequency, 3),
               "state line");
#undef FLUB_VA_IN_LINE
static_assert (offsetof (Control, DataCapacityBytes) % 8 == 0 && offsetof (Control, WriteQpc) % 8 == 0
                   && offsetof (Control, Overruns) % 8 == 0 && offsetof (Control, ReadQpc) % 8 == 0
                   && offsetof (Control, Underruns) % 8 == 0 && offsetof (Control, QpcFrequency) % 8 == 0,
               "64-bit fields must be 8-byte aligned (single-copy atomic on x64 / ARM64)");
static_assert (alignof (Control) <= FLUB_VA_CACHE_LINE_BYTES);

// ---- IOCTL structs: 32/64-bit neutral (no pointers, no size-dependent padding) ---
static_assert (sizeof (FLUB_VA_ENDPOINT_INFO) % 8 == 0 && sizeof (FLUB_VA_DRIVER_INFO) % 8 == 0
                   && sizeof (FLUB_VA_MAP_REQUEST) % 8 == 0 && sizeof (FLUB_VA_MAP_RESULT) % 8 == 0
                   && sizeof (FLUB_VA_UNMAP_REQUEST) % 8 == 0 && sizeof (FLUB_VA_CLOCK_REQUEST) % 8 == 0,
               "IOCTL buffers keep the same size for 32- and 64-bit callers");
static_assert (offsetof (FLUB_VA_ENDPOINT_INFO, DataCapacityBytes) == 24);
static_assert (offsetof (FLUB_VA_DRIVER_INFO, Endpoints) == 16);
static_assert (sizeof (FLUB_VA_DRIVER_INFO::Endpoints) / sizeof (FLUB_VA_ENDPOINT_INFO) == FLUB_VA_ENDPOINT_COUNT);
static_assert (offsetof (FLUB_VA_MAP_REQUEST, DataEvent) == 8 && offsetof (FLUB_VA_MAP_REQUEST, StateEvent) == 16,
               "64-bit handle fields");
static_assert (offsetof (FLUB_VA_MAP_RESULT, DataAddress) == 8 && offsetof (FLUB_VA_MAP_RESULT, DataCapacityBytes) == 16
                   && offsetof (FLUB_VA_MAP_RESULT, ControlBytes) == 24 && offsetof (FLUB_VA_MAP_RESULT, Generation) == 28,
               "64-bit address fields first");

// ---- IOCTL codes: CTL_CODE (DeviceType, Function, Method, Access) -------------------
constexpr FLUB_VA_U32 deviceTypeOf (FLUB_VA_U32 code) noexcept { return code >> 16; }
constexpr FLUB_VA_U32 accessOf (FLUB_VA_U32 code) noexcept { return (code >> 14) & 0x3u; }
constexpr FLUB_VA_U32 functionOf (FLUB_VA_U32 code) noexcept { return (code >> 2) & 0xFFFu; }
constexpr FLUB_VA_U32 methodOf (FLUB_VA_U32 code) noexcept { return code & 0x3u; }

constexpr FLUB_VA_U32 kIoctls[] = { IOCTL_FLUB_VA_GET_INFO, IOCTL_FLUB_VA_MAP_ENDPOINT, IOCTL_FLUB_VA_UNMAP_ENDPOINT,
                                    IOCTL_FLUB_VA_SET_CLOCK_MODE };

constexpr bool ioctlsAreWellFormed() noexcept
{
    for (const auto code : kIoctls)
    {
        if (deviceTypeOf (code) != FLUB_VA_DEVICE_TYPE || methodOf (code) != FLUB_VA_METHOD_BUFFERED)
            return false;
        if (functionOf (code) < 0x800u) // 0x000-0x7FF is reserved for Microsoft
            return false;
    }
    return true;
}
static_assert (FLUB_VA_DEVICE_TYPE >= 0x8000u && FLUB_VA_DEVICE_TYPE <= 0xFFFFu, "vendor device-type range");
static_assert (ioctlsAreWellFormed());
static_assert (accessOf (IOCTL_FLUB_VA_GET_INFO) == FLUB_VA_FILE_READ_ACCESS, "GET_INFO is read-only");
static_assert (accessOf (IOCTL_FLUB_VA_MAP_ENDPOINT) == FLUB_VA_FILE_RW_ACCESS && accessOf (IOCTL_FLUB_VA_UNMAP_ENDPOINT) == FLUB_VA_FILE_RW_ACCESS
                   && accessOf (IOCTL_FLUB_VA_SET_CLOCK_MODE) == FLUB_VA_FILE_RW_ACCESS,
               "state-changing IOCTLs need write access");

// ---- constants ------------------------------------------------------------------
static_assert (FLUB_VA_MAX_CHANNELS == static_cast<unsigned> (flub::kMaxChannels),
               "the engine's chain must accept every endpoint format (Game = 7.1)");
static_assert (FLUB_VA_ENDPOINT_GAME < FLUB_VA_ENDPOINT_COUNT && FLUB_VA_ENDPOINT_MUSIC < FLUB_VA_ENDPOINT_COUNT
               && FLUB_VA_ENDPOINT_CHAT < FLUB_VA_ENDPOINT_COUNT && FLUB_VA_ENDPOINT_SYSTEM < FLUB_VA_ENDPOINT_COUNT
               && FLUB_VA_ENDPOINT_MIC < FLUB_VA_ENDPOINT_COUNT);
static_assert (FLUB_VA_STATE_STOP == 0 && FLUB_VA_STATE_ACQUIRE == 1 && FLUB_VA_STATE_PAUSE == 2 && FLUB_VA_STATE_RUN == 3,
               "mirrors KSSTATE_STOP / ACQUIRE / PAUSE / RUN");
static_assert (isPowerOfTwo (FLUB_VA_FLAG_DISCONTINUITY) && isPowerOfTwo (FLUB_VA_FLAG_ENGINE_TIMEOUT)
               && (FLUB_VA_FLAG_DISCONTINUITY & FLUB_VA_FLAG_ENGINE_TIMEOUT) == 0);

// ---- ring maths (the memory-ordering contract, single-threaded model) --------------

/** Byte offset of frame `position` in the cyclic buffer, as documented:
    (position % BufferFrames) * BytesPerFrame. A mask would be wrong -
    BufferFrames is not necessarily a power of two. */
FLUB_VA_U64 byteOffset (const Control& c, FLUB_VA_U64 position)
{
    return (position % c.BufferFrames) * c.BytesPerFrame;
}

/** Copies `frames` frames between `linear` and the ring at `position`,
    split at the end of the buffer. */
void copyFrames (const Control& c, std::vector<FLUB_VA_U8>& ring, FLUB_VA_U64 position, FLUB_VA_U8* linear, FLUB_VA_U64 frames,
                 bool intoRing)
{
    const FLUB_VA_U64 ringBytes = static_cast<FLUB_VA_U64> (c.BufferFrames) * c.BytesPerFrame;
    const FLUB_VA_U64 offset = byteOffset (c, position);
    const FLUB_VA_U64 bytes = frames * c.BytesPerFrame;
    const FLUB_VA_U64 first = std::min (bytes, ringBytes - offset);
    REQUIRE (offset < ringBytes && bytes <= ringBytes && ringBytes <= c.DataCapacityBytes);

    FLUB_VA_U8* const ringAt = ring.data() + offset;
    if (intoRing)
    {
        std::memcpy (ringAt, linear, static_cast<size_t> (first));
        std::memcpy (ring.data(), linear + first, static_cast<size_t> (bytes - first));
    }
    else
    {
        std::memcpy (linear, ringAt, static_cast<size_t> (first));
        std::memcpy (linear + first, ring.data(), static_cast<size_t> (bytes - first));
    }
}

/** Sample tag of frame `position`, channel `ch` (to verify order and integrity). */
uint32_t tagOf (FLUB_VA_U64 position, FLUB_VA_U32 ch)
{
    return static_cast<uint32_t> (position * 8u + ch);
}

// ---- the Generation sequence lock -----------------------------------------------
struct Description
{
    FLUB_VA_U32 sampleRate = 0, numChannels = 0, bufferFrames = 0;
};

/** The documented reader: load Generation (acquire), retry while odd, copy,
    load again and retry if it changed. */
bool readDescription (const Control& c, Description& out)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const FLUB_VA_U32 before = c.Generation;
        if ((before & 1u) != 0)
            continue;
        out = { c.SampleRate, c.NumChannels, c.BufferFrames };
        if (c.Generation == before)
            return true;
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
TEST_CASE ("Driver ABI: shared header constants, magic byte order and IOCTL codes")
{
    // "bytes 'F','L','V','A' in memory (little endian)" - x64 and ARM64 Windows.
    if constexpr (std::endian::native == std::endian::little)
    {
        const FLUB_VA_U32 magic = FLUB_VA_MAGIC;
        char bytes[4] = {};
        std::memcpy (bytes, &magic, sizeof (bytes));
        CHECK (bytes[0] == 'F' && bytes[1] == 'L' && bytes[2] == 'V' && bytes[3] == 'A');
    }

    const std::set<FLUB_VA_U32> codes (std::begin (kIoctls), std::end (kIoctls));
    CHECK (codes.size() == std::size (kIoctls));
    CHECK (IOCTL_FLUB_VA_MAP_ENDPOINT == 0x8F1BE008u); // FILE_READ_ACCESS | FILE_WRITE_ACCESS, function 0x802

    const std::set<unsigned> endpoints { FLUB_VA_ENDPOINT_GAME, FLUB_VA_ENDPOINT_MUSIC, FLUB_VA_ENDPOINT_CHAT, FLUB_VA_ENDPOINT_SYSTEM,
                                         FLUB_VA_ENDPOINT_MIC };
    CHECK (endpoints.size() == FLUB_VA_ENDPOINT_COUNT);

    // A zeroed control page (what a fresh mapping holds before the driver
    // fills it) is recognisably invalid, and StructBytes can describe itself.
    const Control zero {};
    CHECK (zero.Magic != FLUB_VA_MAGIC);
    CHECK (zero.Generation % 2u == 0u);
    CHECK (sizeof (Control) == 4u * FLUB_VA_CACHE_LINE_BYTES);
}

TEST_CASE ("Driver ABI: ring index maths over a non power-of-two buffer and 64-bit positions")
{
    // 10 ms at 44.1 kHz, stereo float: 441 frames (not 2^n), 8 bytes/frame,
    // data region rounded up to whole pages as the driver maps it.
    Control c {};
    c.SampleFormat = FLUB_VA_FORMAT_FLOAT32;
    c.NumChannels = 2;
    c.BytesPerFrame = c.NumChannels * 4u;
    c.BufferFrames = 441;
    const FLUB_VA_U64 ringBytes = static_cast<FLUB_VA_U64> (c.BufferFrames) * c.BytesPerFrame;
    c.DataCapacityBytes = (ringBytes + FLUB_VA_PAGE_BYTES - 1) / FLUB_VA_PAGE_BYTES * FLUB_VA_PAGE_BYTES;
    REQUIRE (! isPowerOfTwo (c.BufferFrames));
    REQUIRE (c.DataCapacityBytes >= ringBytes);

    // Start just below 2^32 so the monotonic counters cross it: the maths must
    // stay 64-bit (2^32 is not a multiple of 441, a 32-bit counter would jump).
    const FLUB_VA_U64 start = (FLUB_VA_U64 { 1 } << 32) - 1000u;
    c.WritePosition = start;
    c.ReadPosition = start;

    std::vector<FLUB_VA_U8> ring (static_cast<size_t> (c.DataCapacityBytes), 0);
    std::vector<FLUB_VA_U8> chunk (static_cast<size_t> (ringBytes), 0);
    uint32_t rng = 12345u;
    auto nextChunk = [&rng] {
        rng = rng * 1664525u + 1013904223u; // LCG: deterministic chunk sizes 1..300
        return static_cast<FLUB_VA_U64> (1u + (rng >> 8) % 300u);
    };

    int badFill = 0, badData = 0;
    for (int step = 0; step < 20000; ++step)
    {
        // Producer: write what fits, publish WritePosition afterwards.
        {
            const FLUB_VA_U64 want = nextChunk();
            const FLUB_VA_U64 space = c.BufferFrames - (c.WritePosition - c.ReadPosition);
            const FLUB_VA_U64 n = std::min (want, space);
            if (n < want)
                c.Overruns = c.Overruns + 1;
            const FLUB_VA_U64 w = c.WritePosition;
            for (FLUB_VA_U64 f = 0; f < n; ++f)
                for (FLUB_VA_U32 ch = 0; ch < c.NumChannels; ++ch)
                {
                    const uint32_t tag = tagOf (w + f, ch);
                    std::memcpy (chunk.data() + f * c.BytesPerFrame + ch * 4u, &tag, 4);
                }
            copyFrames (c, ring, w, chunk.data(), n, true);
            c.WritePosition = w + n;
        }
        if (c.WritePosition - c.ReadPosition > c.BufferFrames)
            ++badFill;

        // Consumer: read what is there, publish ReadPosition afterwards.
        {
            const FLUB_VA_U64 want = nextChunk();
            const FLUB_VA_U64 available = c.WritePosition - c.ReadPosition;
            const FLUB_VA_U64 n = std::min (want, available);
            if (n < want)
                c.Underruns = c.Underruns + 1;
            const FLUB_VA_U64 r = c.ReadPosition;
            copyFrames (c, ring, r, chunk.data(), n, false);
            for (FLUB_VA_U64 f = 0; f < n; ++f)
                for (FLUB_VA_U32 ch = 0; ch < c.NumChannels; ++ch)
                {
                    uint32_t tag = 0;
                    std::memcpy (&tag, chunk.data() + f * c.BytesPerFrame + ch * 4u, 4);
                    badData += tag != tagOf (r + f, ch) ? 1 : 0;
                }
            c.ReadPosition = r + n;
            c.ReadOffsetBytes = static_cast<FLUB_VA_U32> (byteOffset (c, c.ReadPosition));
        }
        if (c.WritePosition < c.ReadPosition || c.ReadOffsetBytes >= ringBytes)
            ++badFill;
    }

    CHECK (badFill == 0);
    CHECK (badData == 0);
    CHECK (c.ReadPosition > (FLUB_VA_U64 { 1 } << 32)); // crossed 2^32
    CHECK (c.ReadPosition - start > 100u * c.BufferFrames); // wrapped the buffer many times
    CHECK (c.Overruns > 0u);  // both edges (full and empty) were exercised
    CHECK (c.Underruns > 0u);
}

TEST_CASE ("Driver ABI: Generation sequence lock rejects copies during and across a change")
{
    Control c {};
    c.SampleRate = 48000;
    c.NumChannels = 2;
    c.BufferFrames = 480;

    Description d;
    REQUIRE (readDescription (c, d));
    CHECK (d.sampleRate == 48000u && d.numChannels == 2u && d.bufferFrames == 480u);

    // Driver: odd before changing anything ...
    const FLUB_VA_U32 before = c.Generation;
    c.Generation = c.Generation + 1u;
    c.SampleRate = 96000; // half-way through the rewrite
    // ... a reader that starts now never accepts the torn description, and one
    // that loaded Generation before the change sees it differ afterwards.
    CHECK (! readDescription (c, d));
    CHECK (c.Generation != before);
    c.BufferFrames = 960;
    c.Generation = c.Generation + 1u; // ... next even value when done

    CHECK (c.Generation == before + 2u);
    REQUIRE (readDescription (c, d));
    CHECK (d.sampleRate == 96000u && d.numChannels == 2u && d.bufferFrames == 960u);
}

#if defined(FLUB_TEST_DRIVER_SHARED_C)
extern "C" size_t flubVaLayoutFromC (int index);

TEST_CASE ("Driver ABI: the C compiler lays the shared structs out like C++")
{
    // Same order as the table in test_driver_shared_c.c.
    const size_t cpp[] = {
        sizeof (Control),
        offsetof (Control, Generation),
        offsetof (Control, DataCapacityBytes),
        offsetof (Control, WritePosition),
        offsetof (Control, ProducerFlags),
        offsetof (Control, ReadPosition),
        offsetof (Control, ReadOffsetBytes),
        offsetof (Control, StreamState),
        offsetof (Control, RequestedClockMode),
        offsetof (Control, QpcFrequency),
        sizeof (FLUB_VA_ENDPOINT_INFO),
        sizeof (FLUB_VA_DRIVER_INFO),
        sizeof (FLUB_VA_MAP_REQUEST),
        sizeof (FLUB_VA_MAP_RESULT),
        sizeof (FLUB_VA_UNMAP_REQUEST),
        sizeof (FLUB_VA_CLOCK_REQUEST),
        IOCTL_FLUB_VA_GET_INFO,
        IOCTL_FLUB_VA_MAP_ENDPOINT,
        IOCTL_FLUB_VA_UNMAP_ENDPOINT,
        IOCTL_FLUB_VA_SET_CLOCK_MODE,
    };
    const int n = static_cast<int> (std::size (cpp));
    for (int i = 0; i < n; ++i)
        if (flubVaLayoutFromC (i) != cpp[i])
            reportFailure (__FILE__, __LINE__, "C/C++ layout mismatch at entry " + std::to_string (i));
    CHECK (flubVaLayoutFromC (n) == static_cast<size_t> (-1)); // both tables have the same length
}
#endif
