
#include "Synth.h"
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/draft/webview.h"
#include "clap/ext/gui.h"
#include "clap/ext/latency.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/tail.h"
#include "clap/ext/timer-support.h"
#include "core/messages.h"
#include "core/resources.h"
#include "webview/gui.h"
#include "gui_lines.h"
#include "params.h"
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <cmath>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static_assert (std::atomic<float>::is_always_lock_free);
static_assert (std::atomic<bool>::is_always_lock_free);

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_params_t* hostParams;
    const clap_host_state_t* hostState;  // mark_dirty: a recalled setting changed
    float sampleRate;

    // Each value lives twice: `parameters` on the audio thread, `mainParameters` on the main thread (get_value,
    // state, UI), each with a flag that the other thread has news. The gesture flags carry the UI's drags.
    std::atomic<float> parameters[P_COUNT], mainParameters[P_COUNT];
    std::atomic<bool> changed[P_COUNT], mainChanged[P_COUNT];
    std::atomic<bool> gestureBegin[P_COUNT], gestureEnd[P_COUNT];

    // Recalled settings (main thread): the window's size, in points.
    double guiWidth = 1180, guiHeight = 644; // the panel's 570 x 311 proportions

    // Where the synth's ended notes go: the block's output events and the time in it, set before each call.
    Synth synth;
    const clap_output_events_t* noteEndEvents = nullptr;
    uint32_t noteEndTime = 0;

    float gain; // audio thread: the level, gliding to the Volume parameter

    // The GUI's keys, main thread to audio thread: key << 8 | velocity (0-127, 0 for a note-off); or
    // pageButton | a PageButton.
    static constexpr uint32_t pageNoteSlots = 64, pageButton = 0x80000000u;
    std::atomic<uint32_t> pageNotes[pageNoteSlots];
    std::atomic<uint32_t> pageNotesWritten { 0 }, pageNotesRead { 0 };
    uint32_t pageHeld[4] = {}; // main thread: keys down on the interface
    // The sequencer's state for the GUI, packed (audio thread to main), and what the GUI was last sent.
    std::atomic<uint32_t> sequencerPacked { 0 };
    std::atomic<bool> sequencerDirty { false };
    uint32_t audioSequencerPacked = 0, shownSequencer = 0;
    // The sequence: from the audio thread when loading ends, to it when a state loads, and the main
    // thread's copy, saved with the state.
    picohDSP::Sequence audioSequence, loadedSequence, mainSequence;
    std::atomic<bool> sequenceForMain { false }, sequenceForAudio { false };
    uint32_t audioSequenceVersion = 0;
    // The wheels, 0 pitch and 1 mod: the GUI's lever to the audio thread, and the host's MIDI back to the GUI.
    std::atomic<float> pageWheels[2] = {}, hostWheels[2] = {};
    std::atomic<bool> pageWheelsNew[2] = {}, wheelsDirty { false };
    // The keys sounding, from any source, for the GUI to light.
    std::atomic<uint32_t> heldNotes[4] = {};
    std::atomic<bool> notesDirty { false };
    // Main thread: what the GUI was last sent, so only changes go.
    double shownValues[P_COUNT] = {};
    uint32_t shownNotes[4] = {};
    float shownWheels[2] = {};

    webview::Gui gui;
    bool guiSizesInPoints = true; // clap.gui sizes: points on macOS, pixels elsewhere
    std::atomic<bool> uiDirty; // the host changed a parameter: send the UI the values
};

static void PluginProcessEvent (MyPlugin* plugin, const clap_event_header_t* event);
static void PluginSyncMainToAudio (MyPlugin* plugin, const clap_output_events_t* out);
static bool PluginSyncAudioToMain (MyPlugin* plugin);
static double PluginGetValue (MyPlugin* plugin, uint32_t i);
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message);
static void PluginSendValues (MyPlugin* plugin, bool all);
static void PluginSendNotes (MyPlugin* plugin, bool all);
static void PluginSendWheels (MyPlugin* plugin, bool all);
static void PluginSendSequencer (MyPlugin* plugin, bool all);
static void PluginPageButton (MyPlugin* plugin, uint32_t button);
static void PluginPageReleaseAll (MyPlugin* plugin);
static void PluginPagePlay (MyPlugin* plugin, int key, uint32_t velocity);

// ---- Descriptor and features ----------------------------------------------
static const char* const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_MONO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.picoh-101",
    .name = "PICOH-101",
    .vendor = "Charlie Culbert",
    .url = "https://github.com/charCulbert/picoh-101",
    .manual_url = "",
    .support_url = "",
    .version = PICOH_VERSION,
    .description = "A monophonic synth inspired by the Roland SH-101",
    .features = pluginFeatures,
};

// ---- Audio ports ----------------------------------------------------------

// No input. One stereo output.
static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return isInput ? 0 : 1; },

    .get = [] (const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_audio_port_info_t* info) -> bool
    {
        if (index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        snprintf (info->name, sizeof (info->name), "%s", isInput ? "Audio Input" : "Audio Output");
        return true;
    },
};

// ---- Note ports -----------------------------------------------------------

// One note input: CLAP notes or MIDI.
static const clap_plugin_note_ports_t extensionNotePorts = {
    .count = [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return isInput ? 1 : 0; },

    .get = [] (const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_note_port_info_t* info) -> bool
    {
        if (! isInput || index)
            return false;
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        snprintf (info->name, sizeof (info->name), "%s", "Note Input");
        return true;
    },
};

// ---- Parameters -----------------------------------------------------------

// Stepped parameters with named choices are enums to CLAP, so a host can list their steps.
static clap_param_info_flags PluginParamFlags (const Param& param)
{
    int choices = 0;
    ParamChoices (param.unit, &choices);
    return CLAP_PARAM_IS_AUTOMATABLE | (param.stepped ? CLAP_PARAM_IS_STEPPED : 0) |
           (choices > 0 ? CLAP_PARAM_IS_ENUM : 0);
}

static const clap_plugin_params_t extensionParams = {
    .count = [] (const clap_plugin_t* plugin) -> uint32_t { return P_COUNT; },

    .get_info = [] (const clap_plugin_t* _plugin, uint32_t index, clap_param_info_t* information) -> bool
    {
        if (index >= P_COUNT)
            return false;
        const Param& param = paramTable[index];
        memset (information, 0, sizeof (clap_param_info_t));
        information->id = param.id;
        information->flags = PluginParamFlags (param);
        information->min_value = param.min;
        information->max_value = param.max;
        information->default_value = param.defaultValue;
        snprintf (information->name, sizeof (information->name), "%s", param.name);
        return true;
    },

    .get_value = [] (const clap_plugin_t* _plugin, clap_id id, double* value) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        uint32_t i = (uint32_t) id;
        if (i >= P_COUNT)
            return false;

        *value = PluginGetValue (plugin, i);
        return true;
    },

    .value_to_text = [] (const clap_plugin_t* _plugin, clap_id id, double value, char* display, uint32_t size) -> bool
    { return id < P_COUNT && ParamValueToText (paramById (id).unit, value, paramById (id).min < 0, display, size); },

    .text_to_value = [] (const clap_plugin_t* _plugin, clap_id id, const char* display, double* value) -> bool
    { return id < P_COUNT && ParamTextToValue (paramById (id).unit, display, value); },

    .flush =
        [] (const clap_plugin_t* _plugin, const clap_input_events_t* in, const clap_output_events_t* out)
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        const uint32_t eventCount = in->size (in);

        PluginSyncMainToAudio (plugin, out);

        // Notes the events end (a retrigger) end at the start, as flush has no audio.
        plugin->noteEndEvents = out;
        plugin->noteEndTime = 0;
        for (uint32_t eventIndex = 0; eventIndex < eventCount; eventIndex++)
        {
            PluginProcessEvent (plugin, in->get (in, eventIndex));
        }
        plugin->noteEndEvents = nullptr;
    },
};

// The audio thread's value, unless the main thread has changed it since.
static double PluginGetValue (MyPlugin* plugin, uint32_t i)
{
    return plugin->mainChanged[i] ? plugin->mainParameters[i].load() : plugin->parameters[i].load();
}

// NaN becomes the default, then kept in range; a stepped value is truncated, as CLAP converts them.
static float PluginConstrain (uint32_t i, double value)
{
    const Param& param = paramById (i);
    if (value != value)
        value = param.defaultValue;
    value = fmin (fmax (value, param.min), param.max);
    if (param.stepped)
        value = trunc (value);
    return (float) value;
}

static void PluginApplyParameter (MyPlugin* plugin, uint32_t i)
{
    plugin->synth.setParameter (i, plugin->parameters[i].load (std::memory_order_relaxed));
}

static void PluginSendGesture (const clap_output_events_t* out, uint32_t i, uint16_t type)
{
    clap_event_param_gesture_t event = {};
    event.header.size = sizeof (event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = type;
    event.header.flags = 0;
    event.param_id = i;
    out->try_push (out, &event.header);
}

// The main thread's edits to the audio thread, and to the host as events (in process, or flush).
static void PluginSyncMainToAudio (MyPlugin* plugin, const clap_output_events_t* out)
{
    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        if (plugin->gestureBegin[i].exchange (false, std::memory_order_acquire))
            PluginSendGesture (out, i, CLAP_EVENT_PARAM_GESTURE_BEGIN);

        if (plugin->mainChanged[i].exchange (false, std::memory_order_acquire))
        {
            const float value = (plugin->mainParameters[i].load (std::memory_order_relaxed));
            plugin->parameters[i].store (value, std::memory_order_relaxed);
            PluginApplyParameter (plugin, i);
            clap_event_param_value_t event = {};
            event.header.size = sizeof (event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = 0;
            event.param_id = i;
            event.cookie = NULL;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = value;
            out->try_push (out, &event.header);
        }

        if (plugin->gestureEnd[i].exchange (false, std::memory_order_acquire))
            PluginSendGesture (out, i, CLAP_EVENT_PARAM_GESTURE_END);
    }
}

static bool PluginSyncAudioToMain (MyPlugin* plugin)
{
    bool anyChanged = false;

    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        // An edit not yet sent to the audio thread is newer than the host's value.
        if (plugin->changed[i].exchange (false, std::memory_order_acquire) &&
            ! plugin->mainChanged[i].load (std::memory_order_acquire))
        {
            plugin->mainParameters[i].store (plugin->parameters[i].load (std::memory_order_relaxed),
                                             std::memory_order_relaxed);
            anyChanged = true;
        }
    }

    return anyChanged;
}

// ---- Recalled settings ----------------------------------------------------

// Saved with the state but not CLAP parameters, so hosts don't list or automate them: the window's size
// (guiWidth, guiHeight, in points).

// Tells the host to save the project with a changed setting.
static void PluginSettingChanged (MyPlugin* plugin)
{
    if (plugin->hostState)
        plugin->hostState->mark_dirty (plugin->host);
}

#define GUI_ASPECT_WIDTH (1180.0)
#define GUI_ASPECT_HEIGHT (644.0)
#define GUI_MIN_WIDTH (800)
#define GUI_MIN_HEIGHT (437)
#define GUI_MAX_WIDTH (2400)
#define GUI_MAX_HEIGHT (1310)

// ---- Latency and tail -----------------------------------------------------

static const clap_plugin_latency_t extensionLatency = {
    .get = [] (const clap_plugin_t* plugin) -> uint32_t { return 0; },
};

static const clap_plugin_tail_t extensionTail = {
    .get = [] (const clap_plugin_t* plugin) -> uint32_t { return 0; },
};

// ---- Process: events, then DSP --------------------------------------------

// The synth's ended notes, to the host, at the time set before the call into the synth.
static void PluginNoteEnded (MyPlugin* plugin, const picohDSP::Note& note)
{
    if (! plugin->noteEndEvents)
        return;
    clap_event_note_t event = {};
    event.header.size = sizeof (event);
    event.header.time = plugin->noteEndTime;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_NOTE_END;
    event.header.flags = 0;
    event.key = note.key;
    event.note_id = note.hostId;
    event.channel = note.channel;
    event.port_index = 0;
    plugin->noteEndEvents->try_push (plugin->noteEndEvents, &event.header);
}

// -1 is every key.
static void PluginMarkHeld (MyPlugin* plugin, int key, bool held)
{
    if (key < 0)
    {
        for (auto& word : plugin->heldNotes)
            word.store (0, std::memory_order_relaxed);
    }
    else if (key < 128)
    {
        const uint32_t bit = 1u << (key % 32);
        if (held)
            plugin->heldNotes[key / 32].fetch_or (bit, std::memory_order_relaxed);
        else
            plugin->heldNotes[key / 32].fetch_and (~bit, std::memory_order_relaxed);
    }
    if (! plugin->notesDirty.exchange (true, std::memory_order_acq_rel))
        plugin->host->request_callback (plugin->host);
}

// Audio thread. fromHost: the GUI hears of it.
static void PluginSetWheel (MyPlugin* plugin, int wheel, float value, bool fromHost)
{
    if (wheel == 0)
        plugin->synth.setPitchBend (value);
    else
        plugin->synth.setModWheel (value);
    if (! fromHost)
        return;
    plugin->hostWheels[wheel].store (value, std::memory_order_relaxed);
    if (! plugin->wheelsDirty.exchange (true, std::memory_order_acq_rel))
        plugin->host->request_callback (plugin->host);
}

static void PluginProcessEvent (MyPlugin* plugin, const clap_event_header_t* event)
{
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
    {
        if (event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF ||
            event->type == CLAP_EVENT_NOTE_CHOKE)
        {
            const clap_event_note_t* note = (const clap_event_note_t*) event;
            const picohDSP::Note which { note->channel, note->key, note->note_id };

            // A key of -1 is every note. A choke releases like a note-off.
            PluginMarkHeld (plugin, note->key, event->type == CLAP_EVENT_NOTE_ON);
            if (event->type == CLAP_EVENT_NOTE_ON)
                plugin->synth.noteOn (which);
            else if (note->key == -1)
                plugin->synth.releaseAll();
            else
                plugin->synth.noteOff (which);
        }
        else if (event->type == CLAP_EVENT_MIDI)
        {
            const uint8_t* data = ((const clap_event_midi_t*) event)->data;
            const uint8_t status = data[0] & 0xF0;
            const picohDSP::Note which { (int16_t) (data[0] & 0x0F), (int16_t) data[1] };
            if (status == 0x80 || status == 0x90)
                PluginMarkHeld (plugin, data[1], status == 0x90 && data[2] > 0);
            if (status == 0x90 && data[2] > 0)
                plugin->synth.noteOn (which);
            else if (status == 0x80 || status == 0x90) // a note-on with velocity 0 is a note-off
                plugin->synth.noteOff (which);
            else if (status == 0xB0 && data[1] == 64) // sustain pedal: down from 64
                plugin->synth.setSustainPedal (data[2] >= 64);
            else if (status == 0xB0 && data[1] == 123) // all notes off
                plugin->synth.releaseAll();
            else if (status == 0xB0 && data[1] == 1) // mod wheel
                PluginSetWheel (plugin, 1, data[2] / 127.0f, true);
            else if (status == 0xE0) // pitch bend, 14 bits from the middle
                PluginSetWheel (plugin, 0, ((data[2] << 7 | data[1]) - 8192) / 8192.0f, true);
        }
        else if (event->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* valueEvent = (const clap_event_param_value_t*) event;
            if (valueEvent->param_id >= P_COUNT)
                return;
            uint32_t i = (uint32_t) valueEvent->param_id;
            plugin->parameters[i].store (PluginConstrain (i, valueEvent->value), std::memory_order_relaxed);
            plugin->changed[i].store (true, std::memory_order_release);
            PluginApplyParameter (plugin, i);

            if (! plugin->uiDirty.exchange (true, std::memory_order_acq_rel))
                plugin->host->request_callback (plugin->host);
        }
    }
}

static void PluginRenderAudio (MyPlugin* plugin, uint32_t start, uint32_t end, float* const* output)
{
    // Notes found finished while rendering end at the slice's last frame.
    plugin->noteEndTime = end - 1;
    plugin->synth.render (output[0] + start, output[1] + start, (int) (end - start));
}

// Volume, gliding so a change doesn't click.
static void PluginApplyVolume (MyPlugin* plugin, uint32_t frameCount, float* const* output)
{
    const float volume = plugin->synth.getVolume();
    float gain = plugin->gain;
    for (uint32_t index = 0; index < frameCount; index++)
    {
        gain += (volume - gain) * 0.002f;
        output[0][index] *= gain;
        output[1][index] *= gain;
    }
    plugin->gain = fabsf (volume - gain) < 1e-4f ? volume : gain;
}

// The page's sequencer buttons, through the page-notes queue.
enum PageButton : uint32_t
{
    pageLoad,
    pagePlay,
    pageRest,
    pageLegatoUp,
    pageLegatoDown,
    pageKeyTransposeUp,
    pageKeyTransposeDown
};

static void PluginPageButtonApply (MyPlugin* plugin, uint32_t button)
{
    Synth& synth = plugin->synth;
    switch (button)
    {
        case pageLoad: synth.sequencerLoad(); break;
        case pagePlay: synth.sequencerPlay(); break;
        case pageRest: synth.sequencerRest(); break;
        case pageLegatoUp:
        case pageLegatoDown: synth.setLegato (button == pageLegatoDown); break;
        case pageKeyTransposeUp:
        case pageKeyTransposeDown: synth.setKeyTransposeHeld (button == pageKeyTransposeDown); break;
        default: break;
    }
}

// After each block: the sequencer's state for the GUI, and a sequence just loaded for the main thread.
static void PluginPublishSequencer (MyPlugin* plugin)
{
    const Synth::SequencerState s = plugin->synth.getSequencerState();
    const uint32_t packed = uint32_t (s.mode) | uint32_t (s.position) << 2 | uint32_t (s.length) << 9
                            | uint32_t (s.keyTranspose + 128) << 16;
    bool wake = false;
    if (packed != plugin->audioSequencerPacked)
    {
        plugin->audioSequencerPacked = packed;
        plugin->sequencerPacked.store (packed, std::memory_order_relaxed);
        wake = ! plugin->sequencerDirty.exchange (true, std::memory_order_acq_rel);
    }
    const uint32_t version = plugin->synth.getSequenceVersion();
    if (version != plugin->audioSequenceVersion && ! plugin->sequenceForMain.load (std::memory_order_acquire))
    {
        plugin->audioSequenceVersion = version;
        plugin->audioSequence = plugin->synth.getSequence();
        plugin->sequenceForMain.store (true, std::memory_order_release);
        wake = true;
    }
    if (wake)
        plugin->host->request_callback (plugin->host);
}

static clap_process_status PluginProcess (MyPlugin* plugin, const clap_process_t* process)
{
    plugin->noteEndEvents = process->out_events;
    plugin->noteEndTime = 0;
    PluginSyncMainToAudio (plugin, process->out_events);

    if (plugin->sequenceForAudio.exchange (false, std::memory_order_acquire))
        plugin->synth.setSequence (plugin->loadedSequence);
    const uint32_t written = plugin->pageNotesWritten.load (std::memory_order_acquire);
    for (uint32_t read = plugin->pageNotesRead.load (std::memory_order_relaxed); read != written; ++read)
    {
        const uint32_t entry = plugin->pageNotes[read % MyPlugin::pageNoteSlots].load (std::memory_order_relaxed);
        if (entry & MyPlugin::pageButton)
        {
            PluginPageButtonApply (plugin, entry & 0xFF);
            plugin->pageNotesRead.store (read + 1, std::memory_order_release);
            continue;
        }
        const int16_t key = int16_t (entry >> 8);
        const uint32_t velocity = entry & 0xFF;
        PluginMarkHeld (plugin, key, velocity > 0);
        if (velocity)
            plugin->synth.noteOn (picohDSP::Note { 0, key });
        else
            plugin->synth.noteOff (picohDSP::Note { 0, key });
        plugin->pageNotesRead.store (read + 1, std::memory_order_release);
    }
    for (int wheel = 0; wheel < 2; wheel++)
        if (plugin->pageWheelsNew[wheel].exchange (false, std::memory_order_acquire))
            PluginSetWheel (plugin, wheel, plugin->pageWheels[wheel].load (std::memory_order_relaxed), false);

    assert (process->audio_outputs_count == 1);
    float* const* output = process->audio_outputs[0].data32;

    const uint32_t frameCount = process->frames_count;
    const uint32_t inputEventCount = process->in_events->size (process->in_events);
    uint32_t eventIndex = 0;
    uint32_t nextEventFrame = inputEventCount ? 0 : frameCount;

    for (uint32_t i = 0; i < frameCount;)
    {
        while (eventIndex < inputEventCount && nextEventFrame == i)
        {
            const clap_event_header_t* event = process->in_events->get (process->in_events, eventIndex);

            if (event->time != i)
            {
                nextEventFrame = event->time;
                break;
            }

            plugin->noteEndTime = event->time; // a note it ends, ends here
            PluginProcessEvent (plugin, event);
            eventIndex++;

            if (eventIndex == inputEventCount)
            {
                nextEventFrame = frameCount;
                break;
            }
        }

        if (nextEventFrame > i)
            PluginRenderAudio (plugin, i, nextEventFrame, output);
        i = nextEventFrame;
    }
    plugin->noteEndEvents = nullptr;
    PluginApplyVolume (plugin, frameCount, output);
    PluginPublishSequencer (plugin);

    return CLAP_PROCESS_CONTINUE;
}

// ---- State: parameters and settings ---------------------------------------

// The sequence as hex: a byte a step, "ff" a rest, 80 added to a tied key. Empty: no sequence.
static std::string PluginSequenceText (const picohDSP::Sequence& sequence)
{
    std::string text;
    char hex[3];
    for (int i = 0; i < sequence.length; ++i)
    {
        std::snprintf (hex, sizeof hex, "%02x", sequence.steps[i]);
        text += hex;
    }
    return text;
}

static void PluginSequenceFromText (std::string_view text, picohDSP::Sequence& sequence)
{
    sequence = {};
    for (size_t i = 0; i + 1 < text.size() && sequence.length < picohDSP::Sequence::maxSteps; i += 2)
    {
        unsigned value = 0;
        if (std::sscanf (std::string (text.substr (i, 2)).c_str(), "%2x", &value) != 1)
            break;
        sequence.steps[sequence.length++] = uint8_t (value);
    }
}

// A CBOR map of each parameter's value by key, the settings, and a version.
static const clap_plugin_state_t extensionState = {
    .save = [] (const clap_plugin_t* _plugin, const clap_ostream_t* stream) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        // Takes in the host's changes from the audio thread first.
        PluginSyncAudioToMain (plugin);

        core::Value::Map state { { "version", 1 } };
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Param& param = paramById (i);
            state[param.key] = (double) plugin->mainParameters[i].load (std::memory_order_relaxed);
        }
        state["window width"] = plugin->guiWidth;
        state["window height"] = plugin->guiHeight;
        state["sequence"] = PluginSequenceText (plugin->mainSequence);

        const auto bytes = core::encode (state);
        const unsigned char* data = bytes.data();
        uint64_t remaining = bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write (stream, data, remaining);
            if (written <= 0)
                return false;
            data += written;
            remaining -= (uint64_t) written;
        }
        return true;
    },

    .load = [] (const clap_plugin_t* _plugin, const clap_istream_t* stream) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        std::vector<unsigned char> bytes;
        unsigned char buffer[256];
        for (int64_t read; (read = stream->read (stream, buffer, sizeof (buffer))) > 0;)
        {
            bytes.insert (bytes.end(), buffer, buffer + read);
            if (bytes.size() > core::maxMessageBytes)
                return false;
        }
        auto state = core::decode (bytes.data(), bytes.size());
        if (! state || (*state)["version"].number() < 1)
            return false;

        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Param& param = paramById (i);
            const double value = (*state)[param.key].number (param.defaultValue);
            plugin->mainParameters[i].store (PluginConstrain (i, value), std::memory_order_relaxed);
            plugin->mainChanged[i].store (true, std::memory_order_release);
        }
        PluginSequenceFromText ((*state)["sequence"].text(), plugin->mainSequence);
        plugin->loadedSequence = plugin->mainSequence;
        plugin->sequenceForAudio.store (true, std::memory_order_release);
        plugin->guiWidth =
            fmin (fmax ((*state)["window width"].number (plugin->guiWidth), GUI_MIN_WIDTH), GUI_MAX_WIDTH);
        plugin->guiHeight =
            fmin (fmax ((*state)["window height"].number (plugin->guiHeight), GUI_MIN_HEIGHT), GUI_MAX_HEIGHT);

        if (plugin->hostParams)
            plugin->hostParams->rescan (plugin->host, CLAP_PARAM_RESCAN_VALUES);
        PluginSendValues (plugin, false);
        return true;
    },
};

// ---- UI connection --------------------------------------------------------

// The GUI talks to the plugin through clap.webview: natively through webview::Gui; in a WCLAP the host
// loads the page through get_uri and get_resource.
static const clap_plugin_webview_t extensionWebview = {
    .get_uri = [] (const clap_plugin_t* _plugin, char* uri, uint32_t capacity) -> int32_t
    {
        static const char start[] = "/page/index.html"; // in resources/
        if (capacity)
            snprintf (uri, capacity, "%s", start);
        return sizeof (start);
    },

    .get_resource = [] (const clap_plugin_t* _plugin, const char* path, char* mime, uint32_t mimeCapacity,
                        const clap_ostream_t* stream) -> bool
    {
        auto resource = core::readResource (path);
        if (! resource || resource->mime.size() >= mimeCapacity)
            return false;
        strcpy (mime, resource->mime.c_str());
        const char* bytes = resource->bytes.data();
        uint64_t remaining = resource->bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write (stream, bytes, remaining);
            if (written <= 0)
                return false;
            bytes += written;
            remaining -= (uint64_t) written;
        }
        return true;
    },

    .receive = [] (const clap_plugin_t* _plugin, const void* buffer, uint32_t size) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        auto message = core::decode (buffer, size);
        return message && PluginReceiveMessage (plugin, *message);
    },
};

// clap.gui sizes are points on macOS, pixels elsewhere.
static double PluginPixelsPerPoint (const MyPlugin* plugin)
{
    return plugin->guiSizesInPoints ? 1 : plugin->gui.pixelsPerPoint();
}

static void PluginClampSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const double scale = PluginPixelsPerPoint (plugin);
    const auto clamp = [scale] (uint32_t value, double low, double high)
    { return std::clamp (value, uint32_t (std::ceil (low * scale)), uint32_t (high * scale)); };
    width = clamp (width, GUI_MIN_WIDTH, GUI_MAX_WIDTH);
    // The panel scales as one, so the window keeps its shape.
    height = uint32_t (std::lround (width * GUI_ASPECT_HEIGHT / GUI_ASPECT_WIDTH));
}

// Clamped, so get_size never reports a size set_size refuses.
static void PluginGuiSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const double scale = PluginPixelsPerPoint (plugin);
    width = uint32_t (std::lround (plugin->guiWidth * scale));
    height = uint32_t (std::lround (plugin->guiHeight * scale));
    PluginClampSize (plugin, width, height);
}

static const clap_plugin_gui_t extensionGui = {
    .is_api_supported = [] (const clap_plugin_t* _plugin, const char* api, bool isFloating) -> bool
    { return ((MyPlugin*) _plugin->plugin_data)->gui.isApiSupported (api, isFloating); },

    .get_preferred_api = [] (const clap_plugin_t* _plugin, const char** api, bool* isFloating) -> bool
    { return ((MyPlugin*) _plugin->plugin_data)->gui.getPreferredApi (api, isFloating); },

    .create = [] (const clap_plugin_t* _plugin, const char* api, bool isFloating) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        if (! plugin->gui.create (api, isFloating))
            return false;
        plugin->guiSizesInPoints = strcmp (api, CLAP_WINDOW_API_WIN32) != 0 && strcmp (api, CLAP_WINDOW_API_X11) != 0;
        uint32_t width, height;
        PluginGuiSize (plugin, width, height);
        plugin->gui.setSize (width, height);
        return true;
    },

    .destroy =
        [] (const clap_plugin_t* _plugin)
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        PluginPageReleaseAll (plugin);
        plugin->gui.destroy();
    },

    .set_scale = [] (const clap_plugin_t* _plugin, double scale) -> bool
    {
        return false; // WebViews scale their content themselves
    },

    .get_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool
    {
        PluginGuiSize ((MyPlugin*) _plugin->plugin_data, *width, *height);
        return true;
    },

    .can_resize = [] (const clap_plugin_t* _plugin) -> bool { return true; },

    .get_resize_hints = [] (const clap_plugin_t* _plugin, clap_gui_resize_hints_t* hints) -> bool
    {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = true;
        hints->aspect_ratio_width = 1180;
        hints->aspect_ratio_height = 644;
        return true;
    },

    .adjust_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool
    {
        PluginClampSize ((MyPlugin*) _plugin->plugin_data, *width, *height);
        return true;
    },

    .set_size = [] (const clap_plugin_t* _plugin, uint32_t width, uint32_t height) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        uint32_t allowedWidth = width, allowedHeight = height;
        PluginClampSize (plugin, allowedWidth, allowedHeight);
        if (allowedWidth != width || allowedHeight != height)
            return false;
        const double newWidth = width / PluginPixelsPerPoint (plugin),
                     newHeight = height / PluginPixelsPerPoint (plugin);
        if (newWidth != plugin->guiWidth || newHeight != plugin->guiHeight)
        {
            plugin->guiWidth = newWidth;
            plugin->guiHeight = newHeight;
            PluginSettingChanged (plugin);
        }
        plugin->gui.setSize (width, height);
        return true;
    },

    .set_parent = [] (const clap_plugin_t* _plugin, const clap_window_t* window) -> bool
    { return ((MyPlugin*) _plugin->plugin_data)->gui.setParent (window); },

    .set_transient = [] (const clap_plugin_t* _plugin, const clap_window_t* window) -> bool { return false; },

    .suggest_title = [] (const clap_plugin_t* _plugin, const char* title) {},

    .show = [] (const clap_plugin_t* _plugin) -> bool { return ((MyPlugin*) _plugin->plugin_data)->gui.show(); },

    .hide = [] (const clap_plugin_t* _plugin) -> bool { return ((MyPlugin*) _plugin->plugin_data)->gui.hide(); },
};

// On Linux the host's timer drives the view (webview::Gui::onTimer).
static const clap_plugin_timer_support_t extensionTimerSupport = {
    .on_timer = [] (const clap_plugin_t* _plugin, clap_id timerId)
    { ((MyPlugin*) _plugin->plugin_data)->gui.onTimer (timerId); },
};

// The GUI speaks the lines in protocol/gui_lines.h, one string per webview message (main thread only).
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message)
{
    using Kind = guiLines::Line::Kind;
    const std::string_view text = message.text();
    char buffer[64];
    if (text.empty() || text.size() >= sizeof (buffer))
        return false;
    text.copy (buffer, text.size());
    buffer[text.size()] = 0;
    const guiLines::Line line = guiLines::parse (buffer);

    switch (line.kind)
    {
        case Kind::hello:
            PluginSendValues (plugin, true);
            PluginSendNotes (plugin, true);
            PluginSendWheels (plugin, true);
            PluginSendSequencer (plugin, true);
            return true;
        case Kind::note: PluginPagePlay (plugin, line.id, (uint32_t) std::lround (line.value * 127.0f)); return true;
        case Kind::allNotesOff: PluginPageReleaseAll (plugin); return true;
        case Kind::sequencer: PluginPageButton (plugin, uint32_t (line.id)); return true;
        case Kind::legato: PluginPageButton (plugin, line.value > 0 ? pageLegatoDown : pageLegatoUp); return true;
        case Kind::keyTranspose:
            PluginPageButton (plugin, line.value > 0 ? pageKeyTransposeDown : pageKeyTransposeUp);
            return true;
        case Kind::bend:
        case Kind::mod:
        {
            const int wheel = line.kind == Kind::bend ? 0 : 1;
            plugin->pageWheels[wheel].store (line.value, std::memory_order_relaxed);
            plugin->pageWheelsNew[wheel].store (true, std::memory_order_release);
            return true;
        }
        case Kind::value:
        case Kind::gesture: break;
        default: return false;
    }

    if (line.id < 0 || line.id >= (int) P_COUNT)
        return false;
    const uint32_t i = (uint32_t) line.id;
    if (line.kind == Kind::value)
    {
        plugin->mainParameters[i].store (PluginConstrain (i, line.value), std::memory_order_relaxed);
        plugin->mainChanged[i].store (true, std::memory_order_release);
        // The GUI already shows it: a state load back to the earlier value must send it again.
        plugin->shownValues[i] = PluginGetValue (plugin, i);
    }
    else
        (line.value > 0 ? plugin->gestureBegin[i] : plugin->gestureEnd[i]).store (true, std::memory_order_release);

    // The host then sends these as gesture and value events in process() or flush().
    if (plugin->hostParams)
        plugin->hostParams->request_flush (plugin->host);
    return true;
}

// Main thread. A full queue drops it.
static void PluginPageButton (MyPlugin* plugin, uint32_t button)
{
    const uint32_t written = plugin->pageNotesWritten.load (std::memory_order_relaxed);
    if (written - plugin->pageNotesRead.load (std::memory_order_acquire) >= MyPlugin::pageNoteSlots)
        return;
    plugin->pageNotes[written % MyPlugin::pageNoteSlots].store (MyPlugin::pageButton | button,
                                                                std::memory_order_relaxed);
    plugin->pageNotesWritten.store (written + 1, std::memory_order_release);
}

// A full queue drops it.
static void PluginPagePlay (MyPlugin* plugin, int key, uint32_t velocity)
{
    const uint32_t written = plugin->pageNotesWritten.load (std::memory_order_relaxed);
    if (written - plugin->pageNotesRead.load (std::memory_order_acquire) >= MyPlugin::pageNoteSlots)
        return;
    plugin->pageNotes[written % MyPlugin::pageNoteSlots].store (uint32_t (key) << 8 | velocity,
                                                                std::memory_order_relaxed);
    plugin->pageNotesWritten.store (written + 1, std::memory_order_release);
    const uint32_t bit = 1u << (key % 32);
    if (velocity)
        plugin->pageHeld[key / 32] |= bit;
    else
        plugin->pageHeld[key / 32] &= ~bit;
}

// When the GUI goes away, lets go of its keys.
static void PluginPageReleaseAll (MyPlugin* plugin)
{
    for (int key = 0; key < 128; key++)
        if (plugin->pageHeld[key / 32] >> (key % 32) & 1)
            PluginPagePlay (plugin, key, 0);
}

static void PluginSendLine (MyPlugin* plugin, const char* line)
{
    plugin->gui.send (core::Value (std::string (line)));
}

// The keys that changed since the GUI last heard, or all of them.
static void PluginSendNotes (MyPlugin* plugin, bool all)
{
    char line[16];
    for (int key = 0; key < 128; key++)
    {
        const uint32_t bit = 1u << (key % 32);
        const bool down = plugin->heldNotes[key / 32].load (std::memory_order_relaxed) & bit;
        const bool shown = plugin->shownNotes[key / 32] & bit;
        if (down == shown && ! (all && down))
            continue;
        plugin->shownNotes[key / 32] = down ? plugin->shownNotes[key / 32] | bit : plugin->shownNotes[key / 32] & ~bit;
        guiLines::keyLine (line, sizeof line, key, down);
        PluginSendLine (plugin, line);
    }
}

// The sequencer's state and KEY TRANSPOSE, if they changed since the GUI last heard, or both.
static void PluginSendSequencer (MyPlugin* plugin, bool all)
{
    const uint32_t packed = plugin->sequencerPacked.load (std::memory_order_relaxed);
    const uint32_t shown = plugin->shownSequencer;
    plugin->shownSequencer = packed;
    char line[40];
    if (all || (packed & 0xFFFF) != (shown & 0xFFFF))
    {
        guiLines::sequencerLine (line, sizeof line, int (packed & 3), int (packed >> 2 & 0x7F), int (packed >> 9 & 0x7F));
        PluginSendLine (plugin, line);
    }
    if (all || (packed >> 16) != (shown >> 16))
    {
        guiLines::keyTransposeLine (line, sizeof line, int (packed >> 16) - 128);
        PluginSendLine (plugin, line);
    }
}

// The host's wheels, if they moved since the GUI last heard, or both.
static void PluginSendWheels (MyPlugin* plugin, bool all)
{
    char line[16];
    for (int wheel = 0; wheel < 2; wheel++)
    {
        const float value = plugin->hostWheels[wheel].load (std::memory_order_relaxed);
        if (! all && value == plugin->shownWheels[wheel])
            continue;
        plugin->shownWheels[wheel] = value;
        guiLines::wheelLine (line, sizeof line, wheel == 0 ? 'b' : 'm', value);
        PluginSendLine (plugin, line);
    }
}

// The values that changed since the GUI last heard, or all of them.
static void PluginSendValues (MyPlugin* plugin, bool all)
{
    char line[40];
    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        const double value = PluginGetValue (plugin, i);
        if (! all && value == plugin->shownValues[i])
            continue;
        plugin->shownValues[i] = value;
        guiLines::valueLine (line, sizeof line, (int) i, (float) value);
        PluginSendLine (plugin, line);
    }
}

// ---- The plugin -----------------------------------------------------------

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,

    .init = [] (const clap_plugin* _plugin) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        plugin->hostParams = (const clap_host_params_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_PARAMS);
        plugin->hostState = (const clap_host_state_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_STATE);
        plugin->gui.init (_plugin, plugin->host);

        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Param& param = paramById (i);
            float defaultValue = (float) param.defaultValue;
            plugin->mainParameters[i].store (defaultValue, std::memory_order_relaxed);
            plugin->parameters[i].store (defaultValue, std::memory_order_relaxed);
        }
        plugin->gain = (float) paramById (P_VOLUME).defaultValue;

        plugin->synth.onNoteEnded = [plugin] (const picohDSP::Note& note) { PluginNoteEnded (plugin, note); };
        return true;
    },

    .destroy =
        [] (const clap_plugin* _plugin)
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        plugin->gui.destroy();
        delete plugin;
    },

    .activate = [] (const clap_plugin* _plugin, double sampleRate, uint32_t minimumFramesCount,
                    uint32_t maximumFramesCount) -> bool
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        plugin->sampleRate = (float) sampleRate;

        // Main thread, audio stopped: the synth can allocate.
        plugin->synth.prepare (sampleRate, (int) maximumFramesCount);
        for (uint32_t i = 0; i < P_COUNT; i++)
            PluginApplyParameter (plugin, i);
        return true;
    },

    .deactivate = [] (const clap_plugin* _plugin) {},

    .start_processing = [] (const clap_plugin* _plugin) -> bool { return true; },

    .stop_processing = [] (const clap_plugin* _plugin) {},

    .reset =
        [] (const clap_plugin* _plugin)
    {
        // The host forgets its notes on a reset, so no NOTE_END is sent.
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        plugin->synth.stopAll();
    },

    .process = [] (const clap_plugin* _plugin, const clap_process_t* process) -> clap_process_status
    { return PluginProcess ((MyPlugin*) _plugin->plugin_data, process); },

    .get_extension = [] (const clap_plugin* plugin, const char* id) -> const void*
    {
        if (0 == strcmp (id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp (id, CLAP_EXT_NOTE_PORTS))
            return &extensionNotePorts;
        if (0 == strcmp (id, CLAP_EXT_PARAMS))
            return &extensionParams;
        if (0 == strcmp (id, CLAP_EXT_LATENCY))
            return &extensionLatency;
        if (0 == strcmp (id, CLAP_EXT_TAIL))
            return &extensionTail;
        if (0 == strcmp (id, CLAP_EXT_STATE))
            return &extensionState;
        if (0 == strcmp (id, CLAP_EXT_WEBVIEW))
            return &extensionWebview;
        if (0 == strcmp (id, CLAP_EXT_GUI))
            return &extensionGui;
        if (0 == strcmp (id, CLAP_EXT_TIMER_SUPPORT))
            return &extensionTimerSupport;
        return nullptr;
    },

    // The host changed parameters or wheels, notes started or ended, or the sequencer moved.
    .on_main_thread =
        [] (const clap_plugin* _plugin)
    {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        if (plugin->uiDirty.exchange (false, std::memory_order_acq_rel))
            PluginSendValues (plugin, false);
        if (plugin->notesDirty.exchange (false, std::memory_order_acq_rel))
            PluginSendNotes (plugin, false);
        if (plugin->wheelsDirty.exchange (false, std::memory_order_acq_rel))
            PluginSendWheels (plugin, false);
        if (plugin->sequencerDirty.exchange (false, std::memory_order_acq_rel))
            PluginSendSequencer (plugin, false);
        if (plugin->sequenceForMain.load (std::memory_order_acquire))
        {
            plugin->mainSequence = plugin->audioSequence;
            plugin->sequenceForMain.store (false, std::memory_order_release);
            PluginSettingChanged (plugin);
        }
    },
};

const clap_plugin_descriptor_t* getPluginDescriptor()
{
    return &pluginDescriptor;
}

const clap_plugin_t* createPlugin (const clap_host_t* host)
{
    MyPlugin* plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    return &plugin->plugin;
}
