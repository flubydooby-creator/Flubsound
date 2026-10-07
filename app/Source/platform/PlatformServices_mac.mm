// Flubsound Pro - macOS implementation of PlatformServices.h (Objective-C++)
//
// Frameworks the app must link: Carbon (RegisterEventHotKey lives in
// HIToolbox), AppKit (NSWorkspace) - JUCE already links AppKit/Foundation -
// and ServiceManagement (SMAppService, start at login).
//   CMake: target_link_libraries (<app> PRIVATE "-framework Carbon" "-framework AppKit"
//                                               "-framework ServiceManagement")
//
// The foreground application (automatic profiles) is
// NSWorkspace.frontmostApplication, queried on the main thread.
//
// Kept deliberately small and conservative: per-app routing and per-process
// capture are documented designs (see below and platform/macos/README.md)
// that report isSupported() == false until they are implemented and tested
// on real hardware.
#if defined(__APPLE__)

#include "PlatformServices.h"
#include "PlatformServicesInternal.h"

#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <vector>

// SMAppService is declared by the macOS 13 SDK and later; older SDKs build
// without start-at-login (AutoStart reports isSupported() == false).
#if __has_include(<ServiceManagement/SMAppService.h>)
    #import <ServiceManagement/ServiceManagement.h>
    #define FLUB_HAVE_SMAPPSERVICE 1
#else
    #define FLUB_HAVE_SMAPPSERVICE 0
#endif

namespace flub::platform
{
namespace
{
//==============================================================================
// GlobalHotkeys - Carbon RegisterEventHotKey
//==============================================================================
/*  Carbon hot keys are still the standard way to get system-wide shortcuts on
    macOS: they need no Accessibility / Input Monitoring permission (unlike
    CGEventTap or NSEvent global monitors) and work in sandboxed apps.
    The handler is installed on the application event target, so
    kEventHotKeyPressed is delivered on the main thread by the normal
    NSApplication run loop that JUCE runs - no extra thread or loop.

    Create, use and destroy this object on the main (message) thread. */
class MacGlobalHotkeys final : public GlobalHotkeys
{
public:
    MacGlobalHotkeys()
    {
        EventTypeSpec eventType;
        eventType.eventClass = kEventClassKeyboard;
        eventType.eventKind = kEventHotKeyPressed;

        handlerUpp = NewEventHandlerUPP (&MacGlobalHotkeys::handleHotKeyEvent);
        if (InstallApplicationEventHandler (handlerUpp, 1, &eventType, this, &handlerRef) != noErr)
            handlerRef = nullptr;
    }

    ~MacGlobalHotkeys() override
    {
        unregisterAll();

        if (handlerRef != nullptr)
            RemoveEventHandler (handlerRef);
        if (handlerUpp != nullptr)
            DisposeEventHandlerUPP (handlerUpp);
    }

    using GlobalHotkeys::registerHotkey;
    bool isSupported() const override { return handlerRef != nullptr; }

    /** Carbon hot keys have no user-visible list: 'description' is unused. */
    bool registerHotkey (int id, const KeyChord& chord, const std::string&, std::function<void()> callback) override
    {
        const bool ok = registerChord (id, chord, std::move (callback));
        reportBinding (id, ok ? BindingResult::Status::Registered : BindingResult::Status::Unavailable);
        return ok;
    }

    void unregisterHotkey (int id) override
    {
        const auto it = entries.find (id);
        if (it == entries.end())
            return;

        if (it->second.ref != nullptr)
            UnregisterEventHotKey (it->second.ref);
        entries.erase (it);
    }

    void unregisterAll() override
    {
        for (auto& item : entries)
        {
            if (item.second.ref != nullptr)
                UnregisterEventHotKey (item.second.ref);
        }
        entries.clear();
    }

private:
    bool registerChord (int id, const KeyChord& chord, std::function<void()> callback)
    {
        if (handlerRef == nullptr || ! callback || ! detail::isValidChord (chord))
            return false;

        const int virtualKey = toMacVirtualKey (chord.keyCode);
        if (virtualKey < 0)
            return false; // e.g. F21-F24 do not exist on Mac keyboards

        // Rebinding: release the old chord first (it may be the same chord),
        // but put it back if the new one cannot be registered, so a failed
        // rebind never silently loses a working shortcut.
        const auto previous = entries.find (id);
        if (previous != entries.end())
        {
            UnregisterEventHotKey (previous->second.ref);
            previous->second.ref = nullptr;
        }

        EventHotKeyRef hotKeyRef = nullptr;
        // Unique in the process, not per instance: every instance registers
        // under the same signature and sees every one of its hot key events.
        static std::atomic<UInt32> nativeIdCounter { 1 };
        const UInt32 nativeId = nativeIdCounter.fetch_add (1);

        // Fails with eventHotKeyExistsErr when another app / the system owns
        // the chord. macOS 15+ also rejects chords whose only modifiers are
        // Option or Option+Shift (anti key-logging change in Sequoia).
        if (! registerNative (static_cast<UInt32> (virtualKey), toCarbonModifiers (chord.modifiers), nativeId, hotKeyRef))
        {
            if (previous != entries.end())
            {
                if (! registerNative (previous->second.virtualKey, previous->second.carbonModifiers, previous->second.nativeId, previous->second.ref))
                    entries.erase (previous); // lost in the meantime: do not keep a dead entry
            }
            return false;
        }

        Entry entry;
        entry.ref = hotKeyRef;
        entry.nativeId = nativeId;
        entry.virtualKey = static_cast<UInt32> (virtualKey);
        entry.carbonModifiers = toCarbonModifiers (chord.modifiers);
        entry.callback = std::move (callback);
        entries[id] = std::move (entry);
        return true;
    }

    struct Entry
    {
        EventHotKeyRef ref = nullptr;
        UInt32 nativeId = 0;
        UInt32 virtualKey = 0;      // kept so a failed rebind can restore this chord
        UInt32 carbonModifiers = 0;
        std::function<void()> callback;
    };

    static bool registerNative (UInt32 virtualKey, UInt32 carbonModifiers, UInt32 nativeId, EventHotKeyRef& ref)
    {
        EventHotKeyID hotKeyId;
        hotKeyId.signature = kSignature;
        hotKeyId.id = nativeId;

        ref = nullptr;
        const OSStatus status = RegisterEventHotKey (virtualKey, carbonModifiers, hotKeyId, GetApplicationEventTarget(), 0, &ref);
        if (status != noErr)
            ref = nullptr;
        return ref != nullptr;
    }

    // 'FlbS' built arithmetically to avoid multi-character literal warnings.
    static constexpr OSType kSignature = (OSType ('F') << 24) | (OSType ('l') << 16) | (OSType ('b') << 8) | OSType ('S');

    static UInt32 toCarbonModifiers (uint32_t modifiers)
    {
        UInt32 flags = 0;
        if ((modifiers & KeyChord::Ctrl) != 0)
            flags |= controlKey;
        if ((modifiers & KeyChord::Alt) != 0)
            flags |= optionKey;
        if ((modifiers & KeyChord::Shift) != 0)
            flags |= shiftKey;
        if ((modifiers & KeyChord::Super) != 0)
            flags |= cmdKey;
        return flags;
    }

    /** KeyChord codes (ASCII A-Z / 0-9, F1..F24 = 0x70.., navigation keys) -> kVK_* virtual key
        codes (US ANSI positions; Carbon hot keys match physical keys). */
    static int toMacVirtualKey (uint32_t keyCode)
    {
        static const int letters[26] = { kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
                                         kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
                                         kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
                                         kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z };
        static const int digits[10] = { kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
                                        kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9 };
        static const int functionKeys[20] = { kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,
                                              kVK_F8,  kVK_F9,  kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14,
                                              kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20 };

        if (detail::isLetterKey (keyCode))
            return letters[keyCode - 'A'];
        if (detail::isDigitKey (keyCode))
            return digits[keyCode - '0'];

        const int fn = detail::functionKeyNumber (keyCode);
        if (fn >= 1 && fn <= 20)
            return functionKeys[fn - 1];

        switch (keyCode)
        {
            case 0x20: return kVK_Space;
            case 0x21: return kVK_PageUp;
            case 0x22: return kVK_PageDown;
            case 0x23: return kVK_End;
            case 0x24: return kVK_Home;
            case 0x25: return kVK_LeftArrow;
            case 0x26: return kVK_UpArrow;
            case 0x27: return kVK_RightArrow;
            case 0x28: return kVK_DownArrow;
            case 0x2D: return kVK_Help; // the Insert position on Apple extended keyboards
            case 0x2E: return kVK_ForwardDelete;
            default: break;
        }
        return -1;
    }

    /** Runs the callback registered under 'nativeId'; false if it is not ours. */
    bool dispatch (UInt32 nativeId)
    {
        for (const auto& item : entries)
        {
            if (item.second.nativeId == nativeId)
            {
                // Copy first: the callback may unregister (and destroy) itself.
                const auto callback = item.second.callback;
                try
                {
                    callback();
                }
                catch (...)
                {
                    // Never let an exception unwind through the Carbon event dispatcher.
                }
                return true;
            }
        }
        return false;
    }

    static OSStatus handleHotKeyEvent (EventHandlerCallRef, EventRef event, void* userData)
    {
        EventHotKeyID hotKeyId;
        hotKeyId.signature = 0;
        hotKeyId.id = 0;

        const OSStatus status = GetEventParameter (event,
                                                   kEventParamDirectObject,
                                                   typeEventHotKeyID,
                                                   nullptr,
                                                   sizeof (hotKeyId),
                                                   nullptr,
                                                   &hotKeyId);

        if (status != noErr || hotKeyId.signature != kSignature || userData == nullptr)
            return eventNotHandledErr;

        // Another instance's hot key (same signature): let the event reach
        // the next handler, which is that instance's.
        // Both as OSStatus: noErr and eventNotHandledErr are constants of two
        // different anonymous enums (Apple Clang: -Wdeprecated-anon-enum-enum-conversion).
        return static_cast<MacGlobalHotkeys*> (userData)->dispatch (hotKeyId.id) ? static_cast<OSStatus> (noErr)
                                                                                 : static_cast<OSStatus> (eventNotHandledErr);
    }

    EventHandlerUPP handlerUpp = nullptr;
    EventHandlerRef handlerRef = nullptr;
    std::map<int, Entry> entries;
};

//==============================================================================
// AppAudioRouter - not available yet on macOS
//==============================================================================
/*  macOS has no per-application output device setting at all, so "routing" an
    app means capturing it with a Core Audio process tap that mutes it on its
    normal output while tapped (see ProcessLoopbackCapture below):

      1. List audio clients: kAudioHardwarePropertyProcessObjectList (macOS 14+)
         -> per AudioObjectID: kAudioProcessPropertyPID, ...BundleID,
         ...IsRunningOutput (-> AudioSessionInfo.isActive).
      2. "Route to strip X" = start a tap with muteBehavior =
         CATapMutedWhenTapped and feed its frames into strip X of the MixEngine.
         The app keeps playing to the system default device as far as it
         knows, but only Flubsound's processed copy is audible.
      3. "Restore" = destroy the tap (the process is unmuted automatically).

    Alternatively users can pick "Flubsound Game" (Audio Server Plug-in,
    platform/macos) as the output inside apps that have their own device menu.
    Until the tap path is implemented and validated this reports
    isSupported() == false and opens the Sound settings. */
class MacAppAudioRouter final : public AppAudioRouter
{
public:
    bool isSupported() const override { return false; }
    std::vector<AudioSessionInfo> enumerateSessions() override { return {}; }

    std::string cannotMoveReason() const override
    {
        return "macOS has no per-application output setting. Choose \"Flubsound\" as the output device inside the "
               "application, or use a Flubsound device as the system output.";
    }

    bool setAppEndpoint (uint32_t, const std::string&, std::string& error) override
    {
        error = cannotMoveReason();
        return false;
    }

    void openSystemRoutingSettings() override
    {
        @autoreleasepool
        {
            // Opens System Settings > Sound (System Preferences before macOS 13).
            NSURL* url = [NSURL URLWithString: @"x-apple.systempreferences:com.apple.preference.sound"];
            if (url != nil)
                [[NSWorkspace sharedWorkspace] openURL: url];
        }
    }
};

//==============================================================================
// ProcessLoopbackCapture - design only (Core Audio process taps, macOS 14.2+)
//==============================================================================
/*  Planned implementation (not written yet because it cannot be tested
    without macOS 14.2+ hardware and a TCC grant):

      1. pid -> process object: AudioObjectGetPropertyData on
         kAudioObjectSystemObject with kAudioHardwarePropertyTranslatePIDToProcessObject.
      2. CATapDescription* tap = [[CATapDescription alloc]
             initStereoMixdownOfProcesses: @[ @(processObject) ]]
         (or initWithProcesses:andDeviceUID:withStream: for multichannel;
          exclusive = YES with an empty list + our own pid gives "everything but
          Flubsound", the equivalent of Windows' EXCLUDE mode);
         tap.privateTap = YES; tap.muteBehavior = CATapMutedWhenTapped;
         includeProcessTree: add the child processes' objects as well
         (Core Audio has no process-tree flag).
      3. AudioHardwareCreateProcessTap (tap, &tapObjectID).
      4. Private aggregate device containing only the tap:
         AudioHardwareCreateAggregateDevice with
           kAudioAggregateDeviceIsPrivateKey = 1,
           kAudioAggregateDeviceTapListKey = @[ @{ kAudioSubTapUIDKey: tap.UUID.UUIDString,
                                                   kAudioSubTapDriftCompensationKey: @YES } ],
           kAudioAggregateDeviceTapAutoStartKey = 1,
           main sub-device = the current output device (clock source).
      5. Read the format from kAudioTapPropertyFormat, then
         AudioDeviceCreateIOProcIDWithBlock + AudioDeviceStart on the aggregate;
         the IO block converts to interleaved float if needed and calls the
         FrameCallback (RT-safe, same contract as on Windows).
      6. stop(): AudioDeviceStop, AudioDeviceDestroyIOProcID,
         AudioHardwareDestroyAggregateDevice, AudioHardwareDestroyProcessTap.

    Requirements: NSAudioCaptureUsageDescription in Info.plist (the user is
    asked for "System Audio Recording" permission on first use), hardened
    runtime, macOS 14.2+ (check with @available). */
class MacProcessLoopbackCapture final : public ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return false; }

    bool start (uint32_t, bool, double, int, FrameCallback, std::string& error) override
    {
        error = "Per-application capture on macOS (Core Audio process taps) is not available in this version.";
        return false;
    }

    void stop() override {}
    bool isRunning() const override { return false; }
};

//==============================================================================
// AutoStart - SMAppService.mainAppService (Login Items, macOS 13+)
//==============================================================================
/*  Registers the running app bundle as a login item; it then shows up in
    System Settings > General > Login Items, where the user can also remove
    it (status reads back as not enabled then). Older systems report
    unsupported: SMLoginItemSetEnabled needs a bundled helper app and
    LSSharedFileList is deprecated. executablePath is not used. */
class MacAutoStart final : public AutoStart
{
public:
    bool isSupported() const override
    {
#if FLUB_HAVE_SMAPPSERVICE
        if (@available (macOS 13.0, *))
            return true;
#endif
        return false;
    }

    bool isEnabled() const override
    {
#if FLUB_HAVE_SMAPPSERVICE
        if (@available (macOS 13.0, *))
            return [SMAppService mainAppService].status == SMAppServiceStatusEnabled;
#endif
        return false;
    }

    bool setEnabled ([[maybe_unused]] bool shouldStart, const std::string&, std::string& error) override
    {
#if FLUB_HAVE_SMAPPSERVICE
        if (@available (macOS 13.0, *))
        {
            @autoreleasepool
            {
                SMAppService* service = [SMAppService mainAppService];
                const SMAppServiceStatus status = service.status;
                const bool registered = status == SMAppServiceStatusEnabled || status == SMAppServiceStatusRequiresApproval;
                if (shouldStart ? status == SMAppServiceStatusEnabled : ! registered)
                    return true; // already in the requested state (unregistering twice is an error)

                // Registered but waiting for the user's approval (also what
                // the status reads after the user switched the item off in
                // Login Items): only System Settings can finish it, and a
                // second register is commonly refused ("Operation not
                // permitted") without ever sending the user there.
                if (shouldStart && status == SMAppServiceStatusRequiresApproval)
                    return askForApproval (error);

                NSError* nsError = nil;
                const BOOL ok = shouldStart ? [service registerAndReturnError:&nsError] : [service unregisterAndReturnError:&nsError];
                if (! ok)
                {
                    if (shouldStart && service.status == SMAppServiceStatusRequiresApproval)
                        return askForApproval (error);
                    const char* reason = nsError != nil ? nsError.localizedDescription.UTF8String : nullptr;
                    error = std::string (shouldStart ? "Could not add Flubsound Pro to the login items: " : "Could not remove Flubsound Pro from the login items: ")
                            + (reason != nullptr ? reason : "unknown error");
                    return false;
                }

                if (shouldStart && service.status == SMAppServiceStatusRequiresApproval)
                    return askForApproval (error);
                return true;
            }
        }
#endif
        error = "Starting at login needs macOS 13 or later. Add Flubsound Pro in System Settings > Users & Groups > Login Items instead.";
        return false;
    }

private:
#if FLUB_HAVE_SMAPPSERVICE
    /** Sends the user to Login Items, the only place that approves the entry. */
    API_AVAILABLE (macos (13.0)) static bool askForApproval (std::string& error)
    {
        [SMAppService openSystemSettingsLoginItems];
        error = "Allow Flubsound Pro in System Settings > General > Login Items to finish.";
        return false;
    }
#endif
};

//==============================================================================
// ForegroundApp - NSWorkspace.frontmostApplication
//==============================================================================
/*  The application that receives key events. NSWorkspace updates it on the
    main run loop, so query from the main (message) thread. Needs no
    Accessibility or Screen Recording permission. The executable path comes
    from executableURL (nil for some agents), the name falls back to the
    localized application name. */
class MacForegroundApp final : public ForegroundApp
{
public:
    bool isSupported() const override { return true; }
    std::string unsupportedReason() const override { return {}; }

    bool query (ForegroundAppInfo& info) override
    {
        @autoreleasepool
        {
            NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
            if (app == nil || app.processIdentifier <= 0)
                return false;

            const auto text = [] (NSString* s) { return s != nil && s.UTF8String != nullptr ? std::string (s.UTF8String) : std::string(); };
            info.processId = static_cast<uint32_t> (app.processIdentifier);
            info.executablePath = app.executableURL != nil ? text (app.executableURL.path) : std::string();
            info.executableName = app.executableURL != nil ? text (app.executableURL.lastPathComponent) : text (app.localizedName);
            info.bundleId = text (app.bundleIdentifier);
            info.isThisProcess = app.processIdentifier == [NSProcessInfo processInfo].processIdentifier;
            return ! info.executableName.empty() || ! info.bundleId.empty();
        }
    }
};

// Non-null token returned by promoteAudioThread on success.
char timeConstraintToken = 0;
} // namespace

//==============================================================================
// AudioEndpoints - Core Audio transport type of the named output device
//==============================================================================
EndpointTransport AudioEndpoints::queryOutputTransport (const std::string& deviceName)
{
    if (deviceName.empty())
        return EndpointTransport::Unknown;

    AudioObjectPropertyAddress devicesAddress { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size) != noErr || size == 0)
        return EndpointTransport::Unknown;

    std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size, ids.data()) != noErr)
        return EndpointTransport::Unknown;

    for (const AudioObjectID id : ids)
    {
        // JUCE names CoreAudio devices by kAudioObjectPropertyName.
        AudioObjectPropertyAddress nameAddress { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof (cfName);
        if (AudioObjectGetPropertyData (id, &nameAddress, 0, nullptr, &nameSize, &cfName) != noErr || cfName == nullptr)
            continue;

        char buffer[512] = {};
        const bool converted = CFStringGetCString (cfName, buffer, sizeof (buffer), kCFStringEncodingUTF8);
        CFRelease (cfName); // the property returns a +1 reference
        if (! converted || deviceName != buffer)
            continue;

        AudioObjectPropertyAddress transportAddress { kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal,
                                                      kAudioObjectPropertyElementMain };
        UInt32 transport = 0;
        UInt32 transportSize = sizeof (transport);
        if (AudioObjectGetPropertyData (id, &transportAddress, 0, nullptr, &transportSize, &transport) != noErr)
            return EndpointTransport::Unknown;

        switch (transport)
        {
            case kAudioDeviceTransportTypeBluetooth:
            case kAudioDeviceTransportTypeBluetoothLE: return EndpointTransport::Bluetooth; // HFP is detected by format (8/16 kHz)
            case kAudioDeviceTransportTypeUSB: return EndpointTransport::Usb;
            case kAudioDeviceTransportTypeBuiltIn: return EndpointTransport::Analog;
            case kAudioDeviceTransportTypeHDMI:
            case kAudioDeviceTransportTypeDisplayPort: return EndpointTransport::Hdmi;
            case kAudioDeviceTransportTypeVirtual:
            case kAudioDeviceTransportTypeAggregate: return EndpointTransport::Virtual;
            default: return EndpointTransport::Unknown;
        }
    }
    return EndpointTransport::Unknown;
}

EndpointVolume AudioEndpoints::queryOutputVolume (const std::string& deviceName)
{
    // docs/11 E32: the output device's volume in dB (the menu bar's slider),
    // of the named device or the default output device.
    EndpointVolume result;
    AudioObjectID device = kAudioObjectUnknown;
    if (! deviceName.empty())
    {
        AudioObjectPropertyAddress devicesAddress { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size) == noErr && size > 0)
        {
            std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));
            if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size, ids.data()) == noErr)
                for (const AudioObjectID id : ids)
                {
                    AudioObjectPropertyAddress nameAddress { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                                             kAudioObjectPropertyElementMain };
                    CFStringRef cfName = nullptr;
                    UInt32 nameSize = sizeof (cfName);
                    if (AudioObjectGetPropertyData (id, &nameAddress, 0, nullptr, &nameSize, &cfName) != noErr || cfName == nullptr)
                        continue;
                    char buffer[512] = {};
                    const bool converted = CFStringGetCString (cfName, buffer, sizeof (buffer), kCFStringEncodingUTF8);
                    CFRelease (cfName);
                    if (converted && deviceName == buffer)
                    {
                        device = id;
                        break;
                    }
                }
        }
    }
    if (device == kAudioObjectUnknown)
    {
        AudioObjectPropertyAddress defaultAddress { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                                    kAudioObjectPropertyElementMain };
        UInt32 size = sizeof (device);
        if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &defaultAddress, 0, nullptr, &size, &device) != noErr
            || device == kAudioObjectUnknown)
        {
            result.error = "No default output device";
            return result;
        }
    }

    // The main element when the device has a master volume, else the mean of
    // the first two channels (many built-in and USB devices only have those).
    const auto readDb = [device] (AudioObjectPropertyElement element, Float32& db) {
        AudioObjectPropertyAddress address { kAudioDevicePropertyVolumeDecibels, kAudioObjectPropertyScopeOutput, element };
        UInt32 size = sizeof (db);
        return AudioObjectHasProperty (device, &address) && AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, &db) == noErr;
    };
    Float32 db = 0.0f, left = 0.0f, right = 0.0f;
    if (readDb (kAudioObjectPropertyElementMain, db))
        result.known = true;
    else if (readDb (1, left) && readDb (2, right))
    {
        db = 0.5f * (left + right);
        result.known = true;
    }
    if (! result.known)
    {
        result.error = "The output device has no volume control";
        return result;
    }
    result.volumeDb = std::isfinite (db) ? std::max (static_cast<float> (db), EndpointVolume::kSilentDb) : EndpointVolume::kSilentDb;

    AudioObjectPropertyAddress muteAddress { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 mute = 0, muteSize = sizeof (mute);
    result.muted = AudioObjectHasProperty (device, &muteAddress)
                   && AudioObjectGetPropertyData (device, &muteAddress, 0, nullptr, &muteSize, &mute) == noErr && mute != 0;
    return result;
}

//==============================================================================
// SystemTuning
//==============================================================================
bool SystemTuning::disablePowerThrottling()
{
    // No-op: macOS has no EcoQoS-style switch for a process. App Nap does not
    // throttle a process while Core Audio IO is running, and real-time audio
    // threads use the time-constraint policy below (or, better, join the
    // device's os_workgroup - kAudioDevicePropertyIOThreadOSWorkgroup - which
    // JUCE exposes via AudioWorkgroup, so Apple Silicon keeps them on P-cores).
    return true;
}

void* SystemTuning::promoteAudioThread()
{
    /*  Mach time-constraint ("real-time") policy. Without knowing the caller's
        buffer size we describe a typical 256-frame / 48 kHz cycle: period
        5.33 ms, up to half of it for computation, finished within the period.
        Values are converted from nanoseconds to Mach absolute time units
        (1 ns on Intel, 125/3 ns per tick on Apple Silicon). */
    const thread_port_t thread = pthread_mach_thread_np (pthread_self());

    // Core Audio's HAL I/O thread already runs with a time-constraint policy
    // derived from the real buffer size. Overwriting it with the generic
    // 256-frame description below would give large buffers a computation
    // quantum far shorter than one block, and the kernel demotes threads that
    // overrun it. Only promote threads that still have the default policy.
    {
        thread_time_constraint_policy_data_t current {};
        mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        boolean_t isDefault = FALSE;
        if (thread_policy_get (thread, THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t> (&current), &count, &isDefault)
                == KERN_SUCCESS
            && ! isDefault)
            return nullptr;
    }

    mach_timebase_info_data_t timebase;
    if (mach_timebase_info (&timebase) != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0)
        return nullptr;

    const auto msToAbsolute = [&timebase] (double ms)
    { return static_cast<uint32_t> (ms * 1.0e6 * static_cast<double> (timebase.denom) / static_cast<double> (timebase.numer)); };

    constexpr double periodMs = 256.0 * 1000.0 / 48000.0;

    thread_time_constraint_policy_data_t policy;
    policy.period = msToAbsolute (periodMs);
    policy.computation = msToAbsolute (periodMs * 0.5);
    policy.constraint = msToAbsolute (periodMs);
    policy.preemptible = 1;

    const kern_return_t result = thread_policy_set (thread,
                                                    THREAD_TIME_CONSTRAINT_POLICY,
                                                    reinterpret_cast<thread_policy_t> (&policy),
                                                    THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    return result == KERN_SUCCESS ? &timeConstraintToken : nullptr;
}

void SystemTuning::revertAudioThread (void* handle)
{
    if (handle == nullptr)
        return;

    thread_standard_policy_data_t standard;
    standard.no_data = 0;
    thread_policy_set (pthread_mach_thread_np (pthread_self()),
                       THREAD_STANDARD_POLICY,
                       reinterpret_cast<thread_policy_t> (&standard),
                       THREAD_STANDARD_POLICY_COUNT);
}

//==============================================================================
std::unique_ptr<GlobalHotkeys> GlobalHotkeys::create() { return std::make_unique<MacGlobalHotkeys>(); }
std::unique_ptr<AppAudioRouter> AppAudioRouter::create() { return std::make_unique<MacAppAudioRouter>(); }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create() { return std::make_unique<MacProcessLoopbackCapture>(); }
std::unique_ptr<AutoStart> AutoStart::create() { return std::make_unique<MacAutoStart>(); }
std::unique_ptr<ForegroundApp> ForegroundApp::create() { return std::make_unique<MacForegroundApp>(); }
} // namespace flub::platform

#endif // __APPLE__
