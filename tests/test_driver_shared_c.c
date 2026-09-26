/*
 * Compiles platform/windows/driver/FlubVirtualAudioShared.h as strict C89,
 * the way the driver includes it (gcc / clang builds of flub_tests, see
 * tests/CMakeLists.txt). A C++-ism slipping into the header fails here, and
 * the header's own layout pins (FLUB_VA_STATIC_ASSERT, the C89 typedef
 * fallback) are evaluated by a C compiler. test_driver_shared.cpp compares
 * the table below with the C++ layout.
 */
#include "FlubVirtualAudioShared.h"

#include <stddef.h>

size_t flubVaLayoutFromC (int index);

/* Same order as the table in test_driver_shared.cpp; (size_t) -1 past the end. */
size_t flubVaLayoutFromC (int index)
{
    const size_t layout[] = {
        sizeof (FLUB_VA_STREAM_CONTROL),
        offsetof (FLUB_VA_STREAM_CONTROL, Generation),
        offsetof (FLUB_VA_STREAM_CONTROL, DataCapacityBytes),
        offsetof (FLUB_VA_STREAM_CONTROL, WritePosition),
        offsetof (FLUB_VA_STREAM_CONTROL, ProducerFlags),
        offsetof (FLUB_VA_STREAM_CONTROL, ReadPosition),
        offsetof (FLUB_VA_STREAM_CONTROL, ReadOffsetBytes),
        offsetof (FLUB_VA_STREAM_CONTROL, StreamState),
        offsetof (FLUB_VA_STREAM_CONTROL, RequestedClockMode),
        offsetof (FLUB_VA_STREAM_CONTROL, QpcFrequency),
        sizeof (FLUB_VA_ENDPOINT_INFO),
        sizeof (FLUB_VA_DRIVER_INFO),
        sizeof (FLUB_VA_MAP_REQUEST),
        sizeof (FLUB_VA_MAP_RESULT),
        sizeof (FLUB_VA_UNMAP_REQUEST),
        sizeof (FLUB_VA_CLOCK_REQUEST),
        IOCTL_FLUB_VA_GET_INFO,
        IOCTL_FLUB_VA_MAP_ENDPOINT,
        IOCTL_FLUB_VA_UNMAP_ENDPOINT,
        IOCTL_FLUB_VA_SET_CLOCK_MODE
    };
    const int count = (int) (sizeof (layout) / sizeof (layout[0]));
    return index >= 0 && index < count ? layout[index] : (size_t) -1;
}
