
#include <TargetConditionals.h>
#if TARGET_OS_IOS == 1
  #import <UIKit/UIKit.h>
#else
  #import <Cocoa/Cocoa.h>
#endif

#define IPLUG_AUVIEWCONTROLLER IPlugAUViewController_vPolyTrack
#define IPLUG_AUAUDIOUNIT IPlugAUAudioUnit_vPolyTrack
#import <PolyTrackAU/IPlugAUAudioUnit.h>
#import <PolyTrackAU/IPlugAUViewController.h>

//! Project version number for PolyTrackAU.
FOUNDATION_EXPORT double PolyTrackAUVersionNumber;

//! Project version string for PolyTrackAU.
FOUNDATION_EXPORT const unsigned char PolyTrackAUVersionString[];

@class IPlugAUViewController_vPolyTrack;
