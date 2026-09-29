#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@interface SPSubtitleRenderer : NSObject

- (instancetype)initWithDevice:(id<MTLDevice>)device logId:(unsigned)logId;

@property (nonatomic) unsigned spLogId;

- (void)processChunk:(const uint8_t *)data length:(size_t)len ptsUs:(int64_t)ptsUs durationUs:(int64_t)durationUs;

- (void)setCodecPrivate:(NSData *)priv;

// authoredStyles=NO marks an app-owned header whose styles follow the selected font.
- (void)setCodecPrivate:(NSData *)priv authoredStyles:(BOOL)authored;

- (void)loadSubtitleText:(NSString *)text completion:(void (^)(BOOL ok))completion;

- (void)invalidatePendingLoads;

- (id<MTLTexture>)textureForTime:(int64_t)us viewportWidth:(int)vw viewportHeight:(int)vh;

- (void)forceNextSample;

- (void)resetTrack;

- (void)setFontScale:(double)scale;

// Font family for app-styled text tracks; nil or empty keeps the style's font.
- (void)setFontFamily:(NSString *)family;

// Style font and plain text of the events nearest the last rendered time, for
// resolving which fonts the default style draws with. Both are nil for authored
// ASS styles or an empty track. The completion runs on the subtitle queue.
- (void)fetchDefaultFontSample:(void (^)(NSString *styleFont, NSString *sampleText))completion;

@property (nonatomic, readonly) BOOL hasSubtitles;

@property (nonatomic, readonly) CGPoint textureOrigin;

@property (atomic, copy) void (^publishCallback)(void);

@end

FOUNDATION_EXPORT NSString *SPSubtitleInlineTagsToASS(NSString *line);
