//
//  ViewController.m
//  AmpController
//
//  Created by Pavel Tsybulin on 17.02.2021.
//

#import "ViewController.h"
#import "ESManager.h"
#import "ESDevice.h"
#import "F53OSCMessage+ESCommand.h"
#import "VDSPManager.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import "VSXViewController.h"

#define K_MAX_LEVEL_DBM_FREQ 15
#define GAIN_MAX 1.0
#define GAIN_MIN 0.0
#define GAIN_MIN_DEVICE -1.0
#define HANDLE_MIN_POS 0.0
#define HANDLE_MAX_POS 107.0
#define HANDLE_EVENT_THRESHOLD_SEC 0.04

#define USE_VSX 1

#define OUT_L 1
#define OUT_R 2
#define BUS_L 0

// VSX | Effect | Steven Slate
//#define VSX_NAME @"VSX"
//#define VSX_TYPENAME @"Effect"
//#define VSX_MANUFACTURER @"Steven Slate"

@interface ViewController () <DevicesObserver, DeviceObserver> {
    ESDevice *device;
    UInt maxLevelDbmCnt ;
    BOOL handleInUse ;
    NSPoint beginPoint ;
    NSDate *beginDate ;
    NSOperationQueue *gq ;
}

@property (weak) IBOutlet NSView *vMainStrip;
@property (weak) IBOutlet NSButton *btnFreq;
@property (weak) IBOutlet NSImageView *ivIndL;
@property (weak) IBOutlet NSImageView *ivIndR;
@property (weak) IBOutlet NSTextField *lblMaxLevelDbm;
@property (weak) IBOutlet NSTextField *lblDbm;
@property (weak) IBOutlet NSImageView *ivHandle;
@property (weak) IBOutlet NSButton *btnMute;
@property (weak) IBOutlet NSButton *btnVSX;
@property (weak) IBOutlet NSButton *btnOnOff;

- (BOOL)triggerButton:(NSButton *)button ;

@end

static void bypassLsnr(void *inRefCon, AudioUnit inUnit, AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement) {
    ViewController *self = (__bridge ViewController *)inRefCon ;
    
    UInt32 flag = 0 ;
    UInt32 sz = sizeof(UInt32) ;
    OSStatus status = AudioUnitGetProperty(inUnit, inID, inScope, inElement, &flag, &sz) ;
    if (status != noErr) {
        NSLog(@"Plugin AudioUnitGetProperty error: %d", status) ;
    } else {
        self.btnOnOff.state = flag == 0 ? NSControlStateValueOn : NSControlStateValueOff ;
        self.btnOnOff.title = flag == 0 ? @"ON" : @"OFF" ;
        [self triggerButton:self.btnOnOff] ;
    }
}

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    
//    [self.btnVSX resetCursorRects] ;
//    NSCursor *hand = [NSCursor pointingHandCursor] ;
//    [self.btnVSX addCursorRect:self.btnVSX.bounds cursor:hand] ;
//    [hand setOnMouseEntered:YES] ;
//    [self.btnVSX addTrackingRect:self.btnVSX.bounds owner:hand userData:nil assumeInside:YES] ;
//
//    NSCursor *arrow = [NSCursor arrowCursor] ;
//    [self.btnVSX addCursorRect:self.btnVSX.bounds cursor:arrow] ;
//    [arrow setOnMouseExited:YES] ;
//    [self.btnVSX addTrackingRect:self.btnVSX.bounds owner:arrow userData:nil assumeInside:YES] ;

    maxLevelDbmCnt = 0 ;
    handleInUse = NO ;
    gq = [[NSOperationQueue alloc] init] ;
    gq.name = @"IOGainQueue" ;
    gq.maxConcurrentOperationCount = 1 ;
    gq.qualityOfService = NSQualityOfServiceUserInitiated ;

    [self lostMode] ;

    [ESManager.sharedManager addDevicesObserver:self];
    [ESManager.sharedManager addDeviceObserver:self];
    
    [self prepareVDSP] ;
    
}

- (void)viewDidAppear {
    [super viewDidAppear] ;
    [self.view.window setLevel: NSStatusWindowLevel];
}

- (void)setRepresentedObject:(id)representedObject {
    [super setRepresentedObject:representedObject];
}

- (NSColor *)colorWithHex:(int)value {
    return [NSColor colorWithRed:(((CGFloat)((value & 0xFF0000)>>16))/255.0) green:(((CGFloat)((value & 0xFF00)>>8))/255.0) blue:(((CGFloat)(value & 0xFF))/255.0) alpha:1] ;
}

- (BOOL)triggerButton:(NSButton *)button {
    if (button.state == NSControlStateValueOn) {
        button.state = NSControlStateValueOn ;
        button.image = [NSImage imageNamed:@"button_pressed"] ;
        NSMutableAttributedString *colorTitle = [[NSMutableAttributedString alloc] initWithAttributedString:[button attributedTitle]] ;
        NSRange titleRange = NSMakeRange(0, [colorTitle length]) ;
        [colorTitle addAttribute:NSForegroundColorAttributeName value:[self colorWithHex:0xA9662B] range:titleRange] ;
        [button setAttributedTitle:colorTitle] ;

        return YES ;
    } else {
        button.state = NSControlStateValueOff ;
        button.image = [NSImage imageNamed:@"button_normal"] ;
        NSMutableAttributedString *colorTitle = [[NSMutableAttributedString alloc] initWithAttributedString:[button attributedTitle]] ;
        NSRange titleRange = NSMakeRange(0, [colorTitle length]) ;
        [colorTitle addAttribute:NSForegroundColorAttributeName value:[self colorWithHex:0x86939A] range:titleRange] ;
        [button setAttributedTitle:colorTitle] ;

        return NO ;
    }
}

- (void)lostMode {
    self.btnFreq.enabled = NO;
    self.btnFreq.title = @"** kHz" ;
    [self slideBlend:self.ivIndL val:INDICATOR_MIN_LVL] ;
    [self slideBlend:self.ivIndR val:INDICATOR_MIN_LVL] ;
    self.ivIndL.hidden = YES ;
    self.ivIndR.hidden = YES ;
    self.lblDbm.stringValue = @"-60.0" ;
    self.view.window.title = @"🎹" ;
}

- (void)foundMode {
    self.view.window.title = [@"🎹 " stringByAppendingString:device.title] ;
    self.btnFreq.enabled = YES;
    self.ivIndL.hidden = NO ;
    self.ivIndR.hidden = NO ;
    
    [self syncFreqDidChangeManager:ESManager.sharedManager device:self->device] ;
    [self updateGainDbm:nil] ;
    [self updateGain:nil] ;
    [self mappingDidChangeManager:ESManager.sharedManager device:self->device io:self->device.outputs[OUT_L]] ;
}

- (void)slideBlend:(NSView *)blend val:(float)val {
    if (isnan(val)) {
        return ;
    }
    CGFloat blendValue = (val - INDICATOR_MIN_LVL) / (INDICATOR_MAX_LVL - INDICATOR_MIN_LVL) * (blend.frame.size.height - 0) ;
    CGRect maskFrame = CGRectMake(0, 0, blend.frame.size.width, blendValue) ;
    if (blend.layer.mask) {
        blend.layer.mask.frame = maskFrame ;
    } else {
        CALayer *maskLayer = [CALayer new] ;
        maskLayer.frame = maskFrame ;
        maskLayer.backgroundColor = [NSColor blackColor].CGColor ;
        blend.layer.mask = maskLayer ;
    }
    blend.needsDisplay = YES ;
}

- (float)fixLevel:(float)level {
    if (level < INDICATOR_MIN_LVL) {
        level = INDICATOR_MIN_LVL ;
    } else if (level > INDICATOR_MAX_LVL) {
        level = INDICATOR_MAX_LVL ;
    }
    return level ;
}

- (void)updateLevel:(ESInputOutput *)io {
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }
    
    float lvl = [self fixLevel:io.level]  ;
    
    if (io == self->device.outputs[OUT_L]) {
        [self slideBlend:self.ivIndL val:lvl] ;
    } else if (io == self->device.outputs[OUT_R]) {
        [self slideBlend:self.ivIndR val:lvl] ;
    }
    
    if (maxLevelDbmCnt == 0) {
        float maxLvl = fmaxf(self->device.outputs[OUT_L].level, self->device.outputs[OUT_R].level) ;
        self.lblMaxLevelDbm.stringValue = [NSString stringWithFormat:@"%.1f", maxLvl] ;
    }
    
    if (++maxLevelDbmCnt >= K_MAX_LEVEL_DBM_FREQ) {
        maxLevelDbmCnt = 0 ;
    }
}

- (void)updateGainDbm:(ESInputOutput *)io {
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }

    float gainDbm = fmaxf(self->device.outputs[OUT_L].gainDbm, self->device.outputs[OUT_R].gainDbm) ;
    self.lblDbm.stringValue = [NSString stringWithFormat:@"%.1f", gainDbm] ;
}

- (float)fixHandleGain:(float)handleGain {
    if (handleGain < GAIN_MIN) {
        handleGain = GAIN_MIN ;
    } else if (handleGain > GAIN_MAX) {
        handleGain = GAIN_MAX ;
    }
    return handleGain ;
}

- (float)fixGain:(float)gain {
    if (gain < GAIN_MIN_DEVICE) {
        gain = GAIN_MIN_DEVICE ;
    } else if (gain > GAIN_MAX) {
        gain = GAIN_MAX ;
    }
    return gain ;
}

- (void)slideHandle:(float)gain {
    gain = [self fixHandleGain:gain] ;
    float handlePos = gain * (HANDLE_MAX_POS - HANDLE_MIN_POS) / (GAIN_MAX - GAIN_MIN) ;
    self.ivHandle.wantsLayer = YES ;
    self.ivHandle.layer.transform = CATransform3DMakeTranslation(0, handlePos, 0) ;
}

- (void)updateGain:(ESInputOutput *)io {
    if (handleInUse) {
        return ;
    }
    
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }

    float gain = [self fixGain:fmaxf(self->device.outputs[OUT_L].gain, self->device.outputs[OUT_R].gain)] ;
    [self slideHandle:gain] ;
}

- (float)fround:(float)value toNearest:(float)nearest {
    return round(value / nearest) * nearest ;
}

- (IBAction)handleDidPan:(NSPanGestureRecognizer *)sender {
    if (sender.state == NSGestureRecognizerStateBegan) {
        beginDate = [NSDate date] ;
        beginPoint = [sender locationInView:self.ivHandle] ;
        handleInUse = YES ;
        return ;
    }

    if (sender.state != NSGestureRecognizerStateChanged) {
        handleInUse = NO ;
        [self updateGain:nil] ;
        return ;
    }

    if ([[NSDate date] timeIntervalSinceDate:beginDate] < HANDLE_EVENT_THRESHOLD_SEC) {
        return ;
    }

    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }

    NSPoint point = [sender locationInView:self.ivHandle] ;
    float dBmPpx = (self->device.outputs[OUT_L].maxDbm - self->device.outputs[OUT_L].minDbm) / (HANDLE_MAX_POS - HANDLE_MIN_POS) ;
    float oldDbm = self->device.outputs[OUT_L].minDbm + (beginPoint.y - 4) * dBmPpx ;
    float newDbm = [self fround:self->device.outputs[OUT_L].minDbm + (point.y - 4) * dBmPpx toNearest:0.5] ;

    if (fabsf(newDbm - oldDbm) < 0.5) {
        return ;
    }

    beginPoint = point ;
    beginDate = [NSDate date] ;

    [self updateToGainDbm:newDbm] ;
}

- (void)updateToGainDbm:(float)newDbm {
    float newGain = [self fixHandleGain:(newDbm - self->device.outputs[OUT_L].minDbm) / (self->device.outputs[OUT_L].maxDbm - self->device.outputs[OUT_L].minDbm) * (GAIN_MAX - GAIN_MIN)] ;
    [self slideHandle:newGain] ;

    [self sendAndCheckGain:newGain io:self->device.outputs[OUT_L] device:self->device] ;
    [self sendAndCheckGain:newGain io:self->device.outputs[OUT_R] device:self->device] ;
}

- (void)sendAndCheckGain:(float)gain io:(ESInputOutput *)io device:(ESDevice *)device {
    io.lastHumanGainTime = [NSDate date] ;
    
    if (io.lastHumanGain == gain) {
        return ;
    }

    io.lastHumanGain = gain ;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(IO_GAIN_THRESHOLD_SEC * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self->gq addOperationWithBlock:^{
            if (io.gain == io.lastHumanGain) {
                return ;
            }
            if ([io.lastHumanGainTime timeIntervalSinceDate:io.lastDeviceGainTime] > IO_GAIN_THRESHOLD_RESEND) {
                [self sendAndCheckGain:io.lastHumanGain io:io device:device] ;
            }
        }] ;
    });

    [[ESManager sharedManager] sendMessage:
     [F53OSCMessage messageWithCommand:[ESCGain commandForIdx:io.idx kind:io.kind]
                                  args:[NSArray arrayWithObject:[NSNumber numberWithFloat:gain]]]
                                    toHost:device.ip
                                    onPort:device.port
     ] ;
}

- (IBAction)btnDimDidClick:(NSButton *)sender {
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }
    
    float delta = 0 ;
    if ([self triggerButton:sender]) {
        delta = -20.0 ;
    } else {
        delta = 20.0 ;
    }

    float oldGain = fmaxf(self->device.outputs[OUT_L].gain, self->device.outputs[OUT_R].gain) ;
    float newGain = oldGain + (GAIN_MAX - GAIN_MIN) / (self->device.outputs[OUT_L].maxDbm - self->device.outputs[OUT_L].minDbm) * delta ;
    newGain = [self fixGain:newGain] ;

    [self sendAndCheckGain:newGain io:self->device.outputs[OUT_L] device:self->device] ;
    [self sendAndCheckGain:newGain io:self->device.outputs[OUT_R] device:self->device] ;
}

- (IBAction)btnMonoDidClick:(NSButton *)sender {
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }
    
    if ([self triggerButton:sender]) {
        
    } else {
        
    }
}

- (void)updateMute:(ESDevice *)device io:(ESInputOutput *)io on:(BOOL)on {
    [io setMute:on] ;
    [[ESManager sharedManager] sendMessage:
     [F53OSCMessage messageWithCommand:(io.kind == kIOKindInput ? [ESCInputMapping commandForIdx:io.idx] : [ESCOutputMapping commandForIdx:io.idx])
                                  args:[NSArray arrayWithObject:[NSNumber numberWithUnsignedInteger:io.mapping]]]
                                    toHost:device.ip
                                    onPort:device.port
     ] ;
}

- (IBAction)btnMuteDidClick:(NSButton *)sender {
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }

    if ([self triggerButton:sender]) {
        sender.state = NSControlStateValueOff ;
        [self triggerButton:sender] ;
        [self updateMute:self->device io:self->device.outputs[OUT_L] on:YES] ;
        [self updateMute:self->device io:self->device.outputs[OUT_R] on:YES] ;
    } else {
        sender.state = NSControlStateValueOff ;
        [self triggerButton:sender] ;
        [self updateMute:self->device io:self->device.outputs[OUT_L] on:NO] ;
        [self updateMute:self->device io:self->device.outputs[OUT_R] on:NO] ;
    }
}

#pragma mark - Mouse Scroll

- (void)scrollWheel:(NSEvent *)event {
    if (!self->device) {
        return;
    }
    
    if (self->device.outputs.count < 3) {
        return;
    }
    
    NSPoint mp = [self.vMainStrip convertPoint:event.locationInWindow fromView:nil] ;
    if (![self.vMainStrip mouse:mp inRect:self.ivHandle.frame]) {
        return [super scrollWheel:event] ;
    }

    if ([[NSDate date] timeIntervalSinceDate:beginDate] < HANDLE_EVENT_THRESHOLD_SEC) {
        return ;
    }

    beginDate = [NSDate date] ;

    float dBmPpx = (self->device.outputs[OUT_L].maxDbm - self->device.outputs[OUT_L].minDbm) / (HANDLE_MAX_POS - HANDLE_MIN_POS) ;
    float gainPpx = (GAIN_MAX - GAIN_MIN) / (HANDLE_MAX_POS - HANDLE_MIN_POS) ;

    float deltaDbm = event.scrollingDeltaY * 0.5 ;
    float deltaGain = deltaDbm * (gainPpx / dBmPpx) ;
    float newGain = [self fixHandleGain:(fmaxf(self->device.outputs[OUT_L].gain, self->device.outputs[OUT_R].gain) + deltaGain)]  ;

    [self sendAndCheckGain:newGain io:self->device.outputs[OUT_L] device:self->device] ;
    [self sendAndCheckGain:newGain io:self->device.outputs[OUT_R] device:self->device] ;
}

//MARK: - Sync Freq

- (IBAction)btnFreqDidClick:(NSButton *)sender {
    [sender.menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, 0) inView:sender] ;
}

- (IBAction)mnuFreqDidClick:(NSMenuItem *)sender {
    [[ESManager sharedManager] sendMessage:
     [F53OSCMessage messageWithCommand:[ESCSyncFrequency command]
                                  args:[NSArray arrayWithObject:[NSNumber numberWithInteger:sender.tag]]]
                                    toHost:self->device.ip
                                    onPort:self->device.port
     ] ;
}

//MARK: - VSX

- (void)showMessageBoxTitle:(NSString *)title message:(NSString *)message {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:title];
    [alert setInformativeText:message];
    [alert addButtonWithTitle:@"OK"] ;
    [alert runModal];
}

- (void)prepareVDSP {
    VDSPManager *mgr = VDSPManager.sharedManager ;
    mgr.sampleFreq = 48000 ;
    [mgr setInput:0 forBus:0 stereo:YES] ;
    [mgr setOutput:BUS_L forBus:0 stereo:YES] ;
    
#ifdef USE_VSX
    AudioComponentDescription componentDescription = {
        .componentType = 'aufx',
        .componentSubType = 'vsxp',
        .componentManufacturer = 'SlAu',
        .componentFlags = 0,
        .componentFlagsMask = 0
    } ;
#else
    AudioComponentDescription componentDescription = {
        .componentType = 'aumf',
        .componentSubType = 'r049',
        .componentManufacturer = 'Bcau',
        .componentFlags = 0,
        .componentFlagsMask = 0
    } ;
#endif
    
    NSArray<AVAudioUnitComponent *> *components = [AVAudioUnitComponentManager.sharedAudioUnitComponentManager componentsMatchingDescription:componentDescription] ;
    
    if (components.count < 1) {
        NSLog(@"VSX not found") ;
        [self showMessageBoxTitle:@"Plugin not found" message:@"VSX Audio Unit plugin not found"] ;
        return;
    }

    AVAudioUnitComponent *vsx = components[0] ;
    
    [AVAudioUnit instantiateWithComponentDescription:vsx.audioComponentDescription options:kAudioComponentInstantiation_LoadInProcess completionHandler:^(__kindof AVAudioUnit * _Nullable audioUnit, NSError * _Nullable error) {
        if (error) {
            NSLog(@"Plugin instantiate error: %@", error) ;
            [self showMessageBoxTitle:@"Plugin error" message:error.description] ;
            return ;
        }

        if (!audioUnit) {
            NSLog(@"VDSPManager.addPlugin error: audioUnit is NULL") ;
            [self showMessageBoxTitle:@"Plugin error" message:@"AudioUnit is NULL"] ;
            return ;
        }

        UInt32 maxFrames = 512 ;
        OSStatus result = AudioUnitSetProperty(audioUnit.audioUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) ;
        if (result != noErr) {
            NSLog(@"VDSPThread.setupPlugin AudioUnitSet BufferFrameSize error: %d", result) ;
        }

        result = AudioUnitInitialize(audioUnit.audioUnit) ;
        if (result != noErr) {
            NSLog(@"AudioUnitInitialize error: %d", result) ;
            [self showMessageBoxTitle:@"Plugin error" message:@"AudioUnitInitialize error"] ;
            return ;
        }
        

        UInt32 flag = 0 ;
        UInt32 sz = sizeof(UInt32) ;
        OSStatus status = AudioUnitGetProperty(audioUnit.audioUnit, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &flag, &sz) ;
        if (status != noErr) {
            NSLog(@"Plugin AudioUnitGetProperty error: %d", status) ;
        } else {
            self.btnOnOff.state = flag == 0 ? NSControlStateValueOn : NSControlStateValueOff ;
            self.btnOnOff.title = flag == 0 ? @"ON" : @"OFF" ;
            [self triggerButton:self.btnOnOff] ;
        }
        
        status = AudioUnitAddPropertyListener(audioUnit.audioUnit, kAudioUnitProperty_BypassEffect, &bypassLsnr, (__bridge void * _Nullable)(self)) ;
        if (status != noErr) {
            NSLog(@"Plugin AudioUnitAddPropertyListener error: %d", status) ;
        }

        VDSPPlugin *plugin = [[VDSPPlugin alloc] init] ;
        plugin.pluginType = kPluginTypeAU ;
        plugin.name = vsx.name ;
        plugin.path = vsx.manufacturerName ;
        plugin.plugin = (void *) CFBridgingRetain(audioUnit) ;
        
        [mgr addPlugin:plugin toBus:0] ;
        [mgr start] ;
    }] ;

}

- (IBAction)btnOnOffDidClick:(NSButton *)sender {
    if ([VDSPManager.sharedManager busWithIdx:0].plugins.count < 1) {
        [self showMessageBoxTitle:@"Plugin not found" message:@"VSX Audio Unit plugin not found"] ;
        return ;
    }
    
    VDSPPlugin *plugin = [VDSPManager.sharedManager busWithIdx:0].plugins[0] ;
    if (plugin.pluginType != kPluginTypeAU) {
        [self showMessageBoxTitle:@"Plugin not found" message:@"VSX Audio Unit plugin not found"] ;
        return ;
    }

    AVAudioUnit *vsx = plugin.plugin ;
    sender.title = sender.state == NSControlStateValueOn ? @"ON" : @"OFF" ;
    UInt32 flag = 0 ;
    if ([self triggerButton:sender]) {
        flag = 0 ;
    } else {
        flag = 1 ;
    }
    
    OSStatus status = AudioUnitSetProperty(vsx.audioUnit, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &flag, sizeof(flag)) ;
    if (status != noErr) {
        NSLog(@"Plugin AudioUnitSetProperty error: %d", status) ;
    }
}

- (void)prepareForSegue:(NSStoryboardSegue *)segue sender:(id)sender {
    if ([segue.identifier isEqualToString:@"showvsx"]) {
        AUViewControllerBase *viewController = (AUViewControllerBase *)sender;
        VSXViewController *vvc = (VSXViewController *) segue.destinationController;
        vvc.view.frame = viewController.view.frame;
        [vvc.view addSubview:viewController.view] ;

        [NSLayoutConstraint activateConstraints:@[
            [NSLayoutConstraint constraintWithItem:vvc.view attribute:NSLayoutAttributeLeading relatedBy:NSLayoutRelationEqual toItem:viewController.view attribute:NSLayoutAttributeLeading multiplier:1.0 constant:0],
            [NSLayoutConstraint constraintWithItem:vvc.view attribute:NSLayoutAttributeTrailing relatedBy:NSLayoutRelationEqual toItem:viewController.view attribute:NSLayoutAttributeTrailing multiplier:1.0 constant:0],
            [NSLayoutConstraint constraintWithItem:vvc.view attribute:NSLayoutAttributeTop relatedBy:NSLayoutRelationEqual toItem:viewController.view attribute:NSLayoutAttributeTop multiplier:1.0 constant:0],
            [NSLayoutConstraint constraintWithItem:vvc.view attribute:NSLayoutAttributeBottom relatedBy:NSLayoutRelationEqual toItem:viewController.view attribute:NSLayoutAttributeBottom multiplier:1.0 constant:0]
        ]] ;
    }
}

- (IBAction)vsxDidClick:(id)sender {
    if ([VDSPManager.sharedManager busWithIdx:0].plugins.count > 0) {
        VDSPPlugin *plugin = [VDSPManager.sharedManager busWithIdx:0].plugins[0] ;
        if (plugin.pluginType == kPluginTypeAU) {
            AVAudioUnit *vsx = plugin.plugin ;
            [vsx.AUAudioUnit requestViewControllerWithCompletionHandler:^(AUViewControllerBase * _Nullable viewController) {
                [self performSegueWithIdentifier:@"showvsx" sender:viewController] ;
            }] ;
        } else {
            [self showMessageBoxTitle:@"Plugin not found" message:@"VSX Audio Unit plugin not found"] ;
        }
    } else {
        [self showMessageBoxTitle:@"Plugin not found" message:@"VSX Audio Unit plugin not found"] ;
    }
}


//MARK: - <DevicesObserver>

- (void)deviceDidFoundManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device {
    NSLog(@"deviceDidFoundManager title:%@ ip:%@ sa:%@", device.title, device.ip, device.soundAddress) ;

    dispatch_async(dispatch_get_main_queue(), ^{
        self->device = device;
        self.view.window.title = device.title;
        [self foundMode] ;
    }) ;
}

- (void)deviceDidLostManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device {
    NSLog(@"deviceDidLostManager title:%@ ip:%@ sa:%@", device.title, device.ip, device.soundAddress) ;

    if (!self->device) {
        return;
    }
    
    if ([self->device.ip isEqual:device.ip] || [self->device.soundAddress isEqual:device.soundAddress]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->device = nil;
            [self lostMode] ;
        }) ;
    }
}

//MARK: - <DeviceObserver>

- (BOOL)containsDevice:(nonnull ESDevice *)device {
    return self->device && ([self->device.ip isEqual:device.ip] || [self->device.soundAddress isEqual:device.soundAddress]) ;
}

- (void)gainDbmDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device io:(nonnull ESInputOutput *)io {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self updateGainDbm:io] ;
    }) ;
}

- (void)gainDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device io:(nonnull ESInputOutput *)io {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self updateGain:io] ;
    }) ;
}

- (void)levelDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device io:(nonnull ESInputOutput *)io {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self updateLevel:io] ;
    }) ;
}

- (void)mappingDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device io:(nonnull ESInputOutput *)io {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (io.mute) {
            self.btnMute.state = NSControlStateValueOn ;
            [self triggerButton:self.btnMute] ;
        } else {
            self.btnMute.state = NSControlStateValueOff ;
            [self triggerButton:self.btnMute] ;
        }
    }) ;
}

- (void)overflowDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device io:(nonnull ESInputOutput *)io {
}

- (void)phantomPowerDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device io:(nonnull ESInputOutput *)io { }

- (void)syncFreqDidChangeManager:(nonnull ESManager *)manager device:(nonnull ESDevice *)device {
    if (!self->device) {
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        switch (self->device.syncFrequency) {
            case 44100:
                self.btnFreq.title = @"44 kHz" ;
                break;
            case 48000:
                self.btnFreq.title = @"48 kHz" ;
                break;
            case 88200:
                self.btnFreq.title = @"88 kHz" ;
                break;
            case 96000:
                self.btnFreq.title = @"96 kHz" ;
                break;
            case 192000:
                self.btnFreq.title = @"192 kHz" ;
                break;

            default:
                break;
        }
        
        if (self->device.syncFrequency > 48000) {
            [[ESManager sharedManager] sendMessage:
             [F53OSCMessage messageWithCommand:[ESCSyncFrequency command]
                                          args:[NSArray arrayWithObject:[NSNumber numberWithInteger:48000]]]
                                            toHost:self->device.ip
                                            onPort:self->device.port
             ] ;
        }
    }) ;
}

@end
