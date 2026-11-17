// Two VST3 builds from one source:
//  - default: "PolyTrack", an AUDIO EFFECT (insert it on the guitar track; MIDI comes out of it).
//  - PT_INSTRUMENT=1 (msbuild /p:EXTRA_RELEASE_DEFS=PT_INSTRUMENT=1): "PolyTrack Instrument",
//    an instrument with a side-chain input, for hosts that only take MIDI from instruments.
//    It has its own plug-in ID so both can be installed side by side.
#ifndef PT_INSTRUMENT
#define PT_INSTRUMENT 0
#endif

#if PT_INSTRUMENT
#define PLUG_NAME "PolyTrack Instrument"
#define PLUG_UNIQUE_ID 'GrPI'
#else
#define PLUG_NAME "PolyTrack"
#define PLUG_UNIQUE_ID 'GrPT'
#endif
#define PLUG_MFR "Griffin Wojtowicz"
#define PLUG_VERSION_HEX 0x00000302
#define PLUG_VERSION_STR "0.3.2"
#define PLUG_MFR_ID 'GrWj'
#define PLUG_URL_STR "https://github.com/Gwojtowicz"
#define PLUG_EMAIL_STR "spam@me.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 Griffin Wojtowicz"
#define PLUG_CLASS_NAME PolyTrack

#define BUNDLE_NAME "PolyTrack"
#define BUNDLE_MFR "GriffinWojtowicz"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "PolyTrack"

// The instrument build takes the guitar on its side-chain or main input. The effect
// build and the standalone app use plain stereo in/out.
#if defined(VST3_API) && PT_INSTRUMENT
#define PLUG_CHANNEL_IO "2.2-2"
#define PLUG_TYPE 1
#define PT_SIDECHAIN_IN 1
#else
#define PLUG_CHANNEL_IO "2-2"
#define PLUG_TYPE 0
#define PT_SIDECHAIN_IN 0
#endif

#define PLUG_LATENCY 0
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 1
#define PLUG_DOES_MPE 1
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 1100
#define PLUG_HEIGHT 660
#define PLUG_FPS 30
#define PLUG_SHARED_RESOURCES 1
#define PLUG_HOST_RESIZE 1
#define PLUG_MIN_WIDTH 550
#define PLUG_MIN_HEIGHT 330
#define PLUG_MAX_WIDTH 4400
#define PLUG_MAX_HEIGHT 2640

#define AUV2_ENTRY PolyTrack_Entry
#define AUV2_ENTRY_STR "PolyTrack_Entry"
#define AUV2_FACTORY PolyTrack_Factory
#define AUV2_VIEW_CLASS PolyTrack_View
#define AUV2_VIEW_CLASS_STR "PolyTrack_View"

#define AAX_TYPE_IDS 'GPT1'
#define AAX_TYPE_IDS_AUDIOSUITE 'GPA1'
#define AAX_PLUG_MFR_STR "Griffin Wojtowicz"
#define AAX_PLUG_NAME_STR "PolyTrack\nIPEF"
#define AAX_PLUG_CATEGORY_STR "Effect"
#define AAX_DOES_AUDIOSUITE 0

#if PT_INSTRUMENT
#define VST3_SUBCATEGORY "Instrument|Synth"
#else
#define VST3_SUBCATEGORY "Fx|Analyzer"
#endif

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define ROBOTO_FN "Roboto-Regular.ttf"
