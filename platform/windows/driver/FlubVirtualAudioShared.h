/*
 * Flubsound Virtual Audio - definitions shared by the WaveRT driver (kernel
 * mode) and the Flubsound engine (user mode).
 *
 * Plain C (C89 comments, no C++ features) so it compiles in:
 *   - the driver (WDK, _KERNEL_MODE defined; include <ntddk.h> or <wdm.h> first),
 *   - the engine (MSVC / clang-cl / MinGW, C or C++),
 *   - non-Windows unit tests (layout checks only).
 *
 * What is shared (see README.md, "Zero-copy engine path"):
 *   For every endpoint the driver owns two page-aligned regions that it maps
 *   into the engine process on IOCTL_FLUB_VA_MAP_ENDPOINT:
 *     1. a control page starting with FLUB_VA_STREAM_CONTROL (positions,
 *        format, state), never visible to audiodg;
 *     2. the data region: the WaveRT cyclic buffer that audiodg reads (Mic)
 *        or writes (Game/Music/Chat/System), up to DataCapacityBytes.
 *
 * Memory-ordering contract (identical in kernel and user mode):
 *   - Positions are 64-bit monotonically increasing FRAME counters; the byte
 *     offset of a frame is (position % BufferFrames) * BytesPerFrame. They are
 *     8-byte aligned, so plain 64-bit loads/stores are single-copy atomic on
 *     x64 and ARM64.
 *   - The producer writes samples first, then publishes WritePosition with
 *     RELEASE semantics (WriteRelease64 / InterlockedExchange64).
 *   - The consumer reads WritePosition with ACQUIRE semantics (ReadAcquire64),
 *     reads the samples, then publishes ReadPosition with RELEASE semantics.
 *   - 0 <= WritePosition - ReadPosition <= BufferFrames at all times.
 *   - Producer and consumer fields sit on separate 64-byte cache lines to
 *     avoid false sharing between the audiodg / driver thread and the
 *     engine thread.
 *   - Fields in cache line 0 change only while StreamState != RUNNING; every
 *     change increments Generation (read it before and after copying the
 *     description - seqlock style - and retry on mismatch).
 *
 * Who produces what:
 *   render endpoints  (Game, Music, Chat, System): audiodg writes the buffer,
 *       the driver advances WritePosition from IMiniportWaveRTOutputStream::
 *       SetWritePacket; the ENGINE is the consumer (advances ReadPosition).
 *   capture endpoint  (Mic): the ENGINE is the producer (advances
 *       WritePosition); the driver reports it to audiodg as the capture
 *       position and advances ReadPosition when audiodg releases packets.
 *
 * ABI rules: never reorder or resize fields; append only by consuming
 * reserved bytes and bumping FLUB_VA_ABI_VERSION. The static asserts at the
 * end of this file pin the layout.
 */
#ifndef FLUB_VIRTUAL_AUDIO_SHARED_H
#define FLUB_VIRTUAL_AUDIO_SHARED_H

/* ------------------------------------------------------------------------- */
/* Fixed-width types                                                          */
/* ------------------------------------------------------------------------- */
#if defined(_KERNEL_MODE)
/* WDK: <ntdef.h> types. */
typedef UCHAR FLUB_VA_U8;
typedef ULONG FLUB_VA_U32;
typedef ULONGLONG FLUB_VA_U64;
typedef LONGLONG FLUB_VA_I64;
#else
    #include <stdint.h>
typedef uint8_t FLUB_VA_U8;
typedef uint32_t FLUB_VA_U32;
typedef uint64_t FLUB_VA_U64;
typedef int64_t FLUB_VA_I64;
#endif

#include <stddef.h> /* offsetof */

/* ------------------------------------------------------------------------- */
/* Compile-time assertion usable from C89, C11 and C++                        */
/* ------------------------------------------------------------------------- */
#if defined(__cplusplus)
    #define FLUB_VA_STATIC_ASSERT(cond, msg) static_assert (cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    #define FLUB_VA_STATIC_ASSERT(cond, msg) _Static_assert (cond, msg)
#else
    #define FLUB_VA_CONCAT2(a, b) a##b
    #define FLUB_VA_CONCAT(a, b) FLUB_VA_CONCAT2 (a, b)
    #define FLUB_VA_STATIC_ASSERT(cond, msg) typedef char FLUB_VA_CONCAT (flub_va_static_assert_, __LINE__)[(cond) ? 1 : -1]
#endif

/* ------------------------------------------------------------------------- */
/* Identity and versioning                                                    */
/* ------------------------------------------------------------------------- */
#define FLUB_VA_MAGIC 0x41564C46u /* bytes 'F','L','V','A' in memory (little endian) */
#define FLUB_VA_ABI_VERSION 1u
#define FLUB_VA_CACHE_LINE_BYTES 64u
#define FLUB_VA_PAGE_BYTES 4096u
#define FLUB_VA_MAX_CHANNELS 8u

/* Device interface the driver registers (IoRegisterDeviceInterface) for its
   control device; the engine finds it with CM_Get_Device_Interface_List and
   opens it with CreateFileW. Generated for Flubsound - never reuse. */
#define FLUB_VA_INTERFACE_GUID_STRING "{e968b28c-aea9-4e2c-a2af-5212a5a2c759}"
#if defined(DEFINE_GUID)
DEFINE_GUID (GUID_DEVINTERFACE_FLUB_VIRTUAL_AUDIO, 0xe968b28c, 0xaea9, 0x4e2c, 0xa2, 0xaf, 0x52, 0x12, 0xa5, 0xa2, 0xc7, 0x59);
#endif

/* ------------------------------------------------------------------------- */
/* Endpoints                                                                  */
/* ------------------------------------------------------------------------- */
#define FLUB_VA_ENDPOINT_GAME 0u   /* render, 7.1 (8 ch), fed to the HRTF virtualiser */
#define FLUB_VA_ENDPOINT_MUSIC 1u  /* render, stereo */
#define FLUB_VA_ENDPOINT_CHAT 2u   /* render, stereo (voice chat apps) */
#define FLUB_VA_ENDPOINT_SYSTEM 3u /* render, stereo (everything else / default device) */
#define FLUB_VA_ENDPOINT_MIC 4u    /* capture, stereo: processed microphone for chat apps */
#define FLUB_VA_ENDPOINT_COUNT 5u

#define FLUB_VA_ENDPOINT_NAME_GAME L"Flubsound Game"
#define FLUB_VA_ENDPOINT_NAME_MUSIC L"Flubsound Music"
#define FLUB_VA_ENDPOINT_NAME_CHAT L"Flubsound Chat"
#define FLUB_VA_ENDPOINT_NAME_SYSTEM L"Flubsound System"
#define FLUB_VA_ENDPOINT_NAME_MIC L"Flubsound Mic"

#define FLUB_VA_DIRECTION_RENDER 0u  /* apps -> audiodg -> buffer -> engine */
#define FLUB_VA_DIRECTION_CAPTURE 1u /* engine -> buffer -> audiodg -> apps */

/* Sample format of the cyclic buffer (as negotiated by audiodg). */
#define FLUB_VA_FORMAT_NONE 0u
#define FLUB_VA_FORMAT_FLOAT32 1u /* preferred, default device format */
#define FLUB_VA_FORMAT_PCM16 2u
#define FLUB_VA_FORMAT_PCM24_IN_32 3u /* 24 valid bits, 32-bit container, MSB aligned */
#define FLUB_VA_FORMAT_PCM32 4u

/* Stream state (mirrors KSSTATE_STOP / ACQUIRE / PAUSE / RUN). */
#define FLUB_VA_STATE_STOP 0u
#define FLUB_VA_STATE_ACQUIRE 1u
#define FLUB_VA_STATE_PAUSE 2u
#define FLUB_VA_STATE_RUN 3u

/* Who advances the device clock (README.md, "Clock slaving vs ASRC"). */
#define FLUB_VA_CLOCK_FREE_RUNNING 0u   /* driver timer (QPC); engine must run ASRC */
#define FLUB_VA_CLOCK_ENGINE_SLAVED 1u  /* engine progress drives the position */

/* FLUB_VA_STREAM_CONTROL.Flags */
#define FLUB_VA_FLAG_DISCONTINUITY 0x1u /* producer skipped data (overrun / restart) */
#define FLUB_VA_FLAG_ENGINE_TIMEOUT 0x2u /* driver fell back to free-running: no engine heartbeat */

/* ------------------------------------------------------------------------- */
/* Control block (first bytes of each endpoint's control page)                */
/* ------------------------------------------------------------------------- */
typedef struct FLUB_VA_STREAM_CONTROL
{
    /* --- cache line 0 (offset 0): description. Driver-written, only while
           StreamState != RUN; each change increments Generation. ---------- */
    FLUB_VA_U32 Magic;         /* FLUB_VA_MAGIC */
    FLUB_VA_U32 AbiVersion;    /* FLUB_VA_ABI_VERSION */
    FLUB_VA_U32 StructBytes;   /* sizeof (FLUB_VA_STREAM_CONTROL) */
    FLUB_VA_U32 EndpointIndex; /* FLUB_VA_ENDPOINT_* */
    FLUB_VA_U32 Direction;     /* FLUB_VA_DIRECTION_* */
    FLUB_VA_U32 SampleFormat;  /* FLUB_VA_FORMAT_* */
    FLUB_VA_U32 SampleRate;    /* Hz */
    FLUB_VA_U32 NumChannels;   /* 1..FLUB_VA_MAX_CHANNELS */
    FLUB_VA_U32 ChannelMask;   /* WAVEFORMATEXTENSIBLE.dwChannelMask */
    FLUB_VA_U32 BytesPerFrame; /* NumChannels * bytes per sample */
    FLUB_VA_U32 BufferFrames;  /* current cyclic buffer size in frames (not necessarily 2^n) */
    FLUB_VA_U32 PeriodFrames;  /* audiodg period = notification interval */
    FLUB_VA_U32 ClockMode;     /* FLUB_VA_CLOCK_* currently in effect */
    volatile FLUB_VA_U32 Generation; /* incremented on every description / buffer change */
    FLUB_VA_U64 DataCapacityBytes;   /* size of the mapped data region (>= BufferFrames * BytesPerFrame) */

    /* --- cache line 1 (offset 64): producer-owned ------------------------- */
    volatile FLUB_VA_U64 WritePosition; /* frames ever written (monotonic) */
    volatile FLUB_VA_I64 WriteQpc;      /* QueryPerformanceCounter at the last WritePosition update */
    volatile FLUB_VA_U64 Overruns;      /* times the producer found the ring full */
    volatile FLUB_VA_U32 ProducerFlags; /* FLUB_VA_FLAG_* raised by the producer */
    FLUB_VA_U8 Reserved1[FLUB_VA_CACHE_LINE_BYTES - 28];

    /* --- cache line 2 (offset 128): consumer-owned ------------------------ */
    volatile FLUB_VA_U64 ReadPosition; /* frames ever consumed (monotonic) */
    volatile FLUB_VA_I64 ReadQpc;      /* QueryPerformanceCounter at the last ReadPosition update */
    volatile FLUB_VA_U64 Underruns;    /* times the consumer found too little data */
    volatile FLUB_VA_U32 ConsumerFlags;
    /* ReadPosition as a byte offset inside the buffer, for audiodg: can be
       exposed as the WaveRT position register (KSRTAUDIO_HWREGISTER) so
       audiodg reads the play position without a kernel transition. */
    volatile FLUB_VA_U32 ReadOffsetBytes;
    FLUB_VA_U8 Reserved2[FLUB_VA_CACHE_LINE_BYTES - 32];

    /* --- cache line 3 (offset 192): state / liveness ---------------------- */
    volatile FLUB_VA_U32 StreamState;     /* FLUB_VA_STATE_*, driver-written */
    volatile FLUB_VA_U32 EngineAttached;  /* 1 while an engine has this endpoint mapped (driver-written) */
    volatile FLUB_VA_U32 EngineHeartbeat; /* engine increments once per cycle; driver watches it */
    volatile FLUB_VA_U32 RequestedClockMode; /* engine-written request, applied by the driver */
    FLUB_VA_I64 QpcFrequency;             /* QueryPerformanceFrequency (constant) */
    FLUB_VA_U8 Reserved3[FLUB_VA_CACHE_LINE_BYTES - 24];
} FLUB_VA_STREAM_CONTROL;

/* ------------------------------------------------------------------------- */
/* IOCTL interface (METHOD_BUFFERED; all pointers/handles are 64-bit fields   */
/* so a 32-bit tool on 64-bit Windows sees the same layout)                   */
/* ------------------------------------------------------------------------- */

/* Same bit layout as CTL_CODE() from <winioctl.h> / <devioctl.h>. */
#define FLUB_VA_CTL_CODE(deviceType, function, method, access) \
    (((FLUB_VA_U32) (deviceType) << 16) | ((FLUB_VA_U32) (access) << 14) | ((FLUB_VA_U32) (function) << 2) | (FLUB_VA_U32) (method))

#define FLUB_VA_DEVICE_TYPE 0x8F1Bu     /* vendor range 0x8000-0xFFFF */
#define FLUB_VA_METHOD_BUFFERED 0u      /* METHOD_BUFFERED */
#define FLUB_VA_FILE_READ_ACCESS 0x1u   /* FILE_READ_ACCESS */
#define FLUB_VA_FILE_WRITE_ACCESS 0x2u  /* FILE_WRITE_ACCESS */
#define FLUB_VA_FILE_RW_ACCESS (FLUB_VA_FILE_READ_ACCESS | FLUB_VA_FILE_WRITE_ACCESS)

/* out: FLUB_VA_DRIVER_INFO */
#define IOCTL_FLUB_VA_GET_INFO FLUB_VA_CTL_CODE (FLUB_VA_DEVICE_TYPE, 0x801, FLUB_VA_METHOD_BUFFERED, FLUB_VA_FILE_READ_ACCESS)
/* in: FLUB_VA_MAP_REQUEST, out: FLUB_VA_MAP_RESULT. Maps control page + data
   region into the CALLING process; mappings are torn down on UNMAP or when
   the file handle is cleaned up (IRP_MJ_CLEANUP, in the owner's context). */
#define IOCTL_FLUB_VA_MAP_ENDPOINT FLUB_VA_CTL_CODE (FLUB_VA_DEVICE_TYPE, 0x802, FLUB_VA_METHOD_BUFFERED, FLUB_VA_FILE_RW_ACCESS)
/* in: FLUB_VA_UNMAP_REQUEST */
#define IOCTL_FLUB_VA_UNMAP_ENDPOINT FLUB_VA_CTL_CODE (FLUB_VA_DEVICE_TYPE, 0x803, FLUB_VA_METHOD_BUFFERED, FLUB_VA_FILE_RW_ACCESS)
/* in: FLUB_VA_CLOCK_REQUEST */
#define IOCTL_FLUB_VA_SET_CLOCK_MODE FLUB_VA_CTL_CODE (FLUB_VA_DEVICE_TYPE, 0x804, FLUB_VA_METHOD_BUFFERED, FLUB_VA_FILE_RW_ACCESS)

typedef struct FLUB_VA_ENDPOINT_INFO
{
    FLUB_VA_U32 EndpointIndex;  /* FLUB_VA_ENDPOINT_* */
    FLUB_VA_U32 Direction;      /* FLUB_VA_DIRECTION_* */
    FLUB_VA_U32 MaxChannels;
    FLUB_VA_U32 DefaultChannelMask;
    FLUB_VA_U32 DefaultSampleRate;
    FLUB_VA_U32 StreamState;    /* FLUB_VA_STATE_* right now */
    FLUB_VA_U64 DataCapacityBytes;
} FLUB_VA_ENDPOINT_INFO;

typedef struct FLUB_VA_DRIVER_INFO
{
    FLUB_VA_U32 Magic;       /* FLUB_VA_MAGIC */
    FLUB_VA_U32 AbiVersion;  /* FLUB_VA_ABI_VERSION: engine refuses to map on mismatch */
    FLUB_VA_U32 DriverVersion; /* major << 24 | minor << 16 | build */
    FLUB_VA_U32 EndpointCount;
    FLUB_VA_ENDPOINT_INFO Endpoints[FLUB_VA_ENDPOINT_COUNT];
} FLUB_VA_DRIVER_INFO;

typedef struct FLUB_VA_MAP_REQUEST
{
    FLUB_VA_U32 EndpointIndex;
    FLUB_VA_U32 Flags;         /* reserved, 0 */
    /* Handles to auto-reset events CREATED BY THE ENGINE (never named): the
       driver references them (ObReferenceObjectByHandle, UserMode,
       EVENT_MODIFY_STATE) and signals DataEvent once per period and
       StateEvent on any Generation / StreamState change. 0 = none. */
    FLUB_VA_U64 DataEvent;
    FLUB_VA_U64 StateEvent;
} FLUB_VA_MAP_REQUEST;

typedef struct FLUB_VA_MAP_RESULT
{
    FLUB_VA_U64 ControlAddress;    /* user VA of FLUB_VA_STREAM_CONTROL */
    FLUB_VA_U64 DataAddress;       /* user VA of the data region */
    FLUB_VA_U64 DataCapacityBytes;
    FLUB_VA_U32 ControlBytes;      /* size of the control mapping (FLUB_VA_PAGE_BYTES) */
    FLUB_VA_U32 Generation;        /* Generation at mapping time */
} FLUB_VA_MAP_RESULT;

typedef struct FLUB_VA_UNMAP_REQUEST
{
    FLUB_VA_U32 EndpointIndex;
    FLUB_VA_U32 Reserved;
} FLUB_VA_UNMAP_REQUEST;

typedef struct FLUB_VA_CLOCK_REQUEST
{
    FLUB_VA_U32 EndpointIndex;
    FLUB_VA_U32 ClockMode; /* FLUB_VA_CLOCK_* */
} FLUB_VA_CLOCK_REQUEST;

/* ------------------------------------------------------------------------- */
/* Optional named events                                                      */
/* ------------------------------------------------------------------------- */
/* The engine should pass its own unnamed events in FLUB_VA_MAP_REQUEST (no
   name-squatting surface). For diagnostics tools the driver ALSO creates one
   notification event per endpoint at load time, before any user process can
   claim the name, with an ACL granting SYNCHRONIZE only to interactive users:
     kernel:  L"\\BaseNamedObjects\\Flubsound.VirtualAudio.<n>.State"
     user:    L"Global\\Flubsound.VirtualAudio.<n>.State"  (OpenEventW, SYNCHRONIZE)
   with <n> = FLUB_VA_ENDPOINT_* as a decimal digit. */
#define FLUB_VA_EVENT_NAME_KERNEL_PREFIX L"\\BaseNamedObjects\\Flubsound.VirtualAudio."
#define FLUB_VA_EVENT_NAME_USER_PREFIX L"Global\\Flubsound.VirtualAudio."
#define FLUB_VA_EVENT_NAME_STATE_SUFFIX L".State"

/* ------------------------------------------------------------------------- */
/* Layout pins                                                                */
/* ------------------------------------------------------------------------- */
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_STREAM_CONTROL) == 4 * FLUB_VA_CACHE_LINE_BYTES, "control block must be 4 cache lines");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_STREAM_CONTROL) <= FLUB_VA_PAGE_BYTES, "control block must fit its page");
FLUB_VA_STATIC_ASSERT (offsetof (FLUB_VA_STREAM_CONTROL, DataCapacityBytes) == 56, "description layout");
FLUB_VA_STATIC_ASSERT (offsetof (FLUB_VA_STREAM_CONTROL, WritePosition) == 1 * FLUB_VA_CACHE_LINE_BYTES, "producer line");
FLUB_VA_STATIC_ASSERT (offsetof (FLUB_VA_STREAM_CONTROL, ReadPosition) == 2 * FLUB_VA_CACHE_LINE_BYTES, "consumer line");
FLUB_VA_STATIC_ASSERT (offsetof (FLUB_VA_STREAM_CONTROL, ReadOffsetBytes) == 2 * FLUB_VA_CACHE_LINE_BYTES + 28, "position register");
FLUB_VA_STATIC_ASSERT (offsetof (FLUB_VA_STREAM_CONTROL, StreamState) == 3 * FLUB_VA_CACHE_LINE_BYTES, "state line");
FLUB_VA_STATIC_ASSERT (offsetof (FLUB_VA_STREAM_CONTROL, QpcFrequency) == 3 * FLUB_VA_CACHE_LINE_BYTES + 16, "state line layout");
FLUB_VA_STATIC_ASSERT ((offsetof (FLUB_VA_STREAM_CONTROL, WritePosition) % 8) == 0, "64-bit positions must be 8-byte aligned");
FLUB_VA_STATIC_ASSERT ((offsetof (FLUB_VA_STREAM_CONTROL, ReadPosition) % 8) == 0, "64-bit positions must be 8-byte aligned");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_ENDPOINT_INFO) == 32, "endpoint info layout");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_DRIVER_INFO) == 16 + FLUB_VA_ENDPOINT_COUNT * 32, "driver info layout");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_MAP_REQUEST) == 24, "map request layout");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_MAP_RESULT) == 32, "map result layout");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_UNMAP_REQUEST) == 8, "unmap request layout");
FLUB_VA_STATIC_ASSERT (sizeof (FLUB_VA_CLOCK_REQUEST) == 8, "clock request layout");
FLUB_VA_STATIC_ASSERT (IOCTL_FLUB_VA_GET_INFO == 0x8F1B6004u, "IOCTL code value");

#if defined(CTL_CODE) && defined(METHOD_BUFFERED) && defined(FILE_READ_ACCESS)
/* Cross-check against the SDK/WDK macro when it is available. */
FLUB_VA_STATIC_ASSERT (IOCTL_FLUB_VA_GET_INFO == CTL_CODE (FLUB_VA_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS),
                       "CTL_CODE mismatch");
FLUB_VA_STATIC_ASSERT (IOCTL_FLUB_VA_MAP_ENDPOINT
                           == CTL_CODE (FLUB_VA_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS),
                       "CTL_CODE mismatch");
#endif

#endif /* FLUB_VIRTUAL_AUDIO_SHARED_H */
