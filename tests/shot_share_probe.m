/* Screenshot sharing (games/totk/runtime/shot_upload.h) against the shader inbox's own handlers on 127.0.0.1
   (MacNativeGamesWeb/shader-inbox test/local-server.js), driven by tests/test_shot_share.py. TOTK_BOARD_DIR is a temporary
   folder, TOTK_SHOT_URL the local server. The picture is a 2560x1440 frame of its own (a colour and a gradient). Modes:
     up     the switch off shares nothing; a body the server refuses (400) leaves the folder; on, a picture is shared
            (1920x1080, marked "me", its full JPEG and its thumbnail read back; TOTK_SHOT_SHARED_NOTE posted once, on the
            main queue, for the 截图分享 page), deleted again, and nothing waits;
     down   no server (TOTK_SHOT_URL at a closed port): the picture waits in shot-pending with the exact upload body, and
            no note is posted;
     start  what the down run left is sent by totk_shot_start (TOTK_SHOT_TEST=1: 2 s after start) and the server lists it;
     off    turning the switch off drops what waits.
   Prints one JSON line: {"status": "pass", ...}; any failure exits 1 with the reason on stderr. */
#include "../games/totk/runtime/game_version.h"   /* TOTK_GAME_VERSION, GAME_NAME */
#include "../games/totk/runtime/shot_upload.h"

/* board_upload.h's renderer and frame-record calls: this probe has no renderer and never runs a board round. */
void nvn_board_pacing(int *lock, int *interp) { *lock = *interp = 0; }
void mtl_board_view(int *ow, int *oh, int *sw, int *sh, int *mfx) { *ow = *oh = *sw = *sh = *mfx = 0; }
int mtl_display_hz(void) { return 0; }
void fps_board_start(void) {}
void fps_board_reset(void) {}
int fps_board_counting(void) { return 0; }
int fps_board_snapshot(fps_board_stats *s) { (void)s; return 0; }

static void fail(NSString *why) {
    fprintf(stderr, "FAIL: %s\n", why.UTF8String);
    exit(1);
}
static void expect(BOOL ok, NSString *why) { if (!ok) fail(why); }

/* The frame: 2560x1440, a blue field with a red-to-green gradient across its lower half. */
static CGImageRef probe_frame(void) {
    const size_t w = 2560, h = 1440;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, space, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGContextSetRGBFillColor(c, 0.1, 0.2, 0.8, 1);
    CGContextFillRect(c, CGRectMake(0, 0, w, h));
    CGFloat stops[] = {1, 0, 0, 1, 0, 1, 0, 1};
    CGGradientRef g = CGGradientCreateWithColorComponents(space, stops, NULL, 2);
    CGContextSaveGState(c);
    CGContextClipToRect(c, CGRectMake(0, 0, w, h / 2));
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(w, 0), 0);
    CGContextRestoreGState(c);
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGGradientRelease(g); CGContextRelease(c); CGColorSpaceRelease(space);
    return img;
}

/* Everything the shots' queue has to do now: the picture's block, then the send it queues. */
static void probe_drain(void) {
    for (int i = 0; i < 3; i++) dispatch_sync(totk_shot_worker(), ^{});
}
/* Runs the main queue until *flag or the time is up. */
static void probe_wait(BOOL *flag, double seconds) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!*flag && end.timeIntervalSinceNow > 0) [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    if (!*flag) fail(@"no answer in time");
}
static NSDictionary *probe_list(void) {
    __block NSDictionary *list = nil;
    __block NSString *why = nil;
    __block BOOL came = NO;
    totk_shot_fetch(nil, ^(NSDictionary *data, NSString *error) {
        expect(NSThread.isMainThread, @"the list's answer is not on the main queue");
        list = data; why = error; came = YES;
    });
    probe_wait(&came, 30);
    expect(list != nil, [NSString stringWithFormat:@"the list was not read: %@", why]);
    return list;
}
static NSArray<NSString *> *probe_pending(void) { return totk_shot_pending_files(); }
/* TOTK_SHOT_SHARED_NOTE as the 截图分享 page hears it: counted, and each on the main thread. */
static int probe_announced;
static void probe_listen(void) {
    [NSNotificationCenter.defaultCenter addObserverForName:TOTK_SHOT_SHARED_NOTE object:nil queue:nil usingBlock:^(NSNotification *n) {
        (void)n; expect(NSThread.isMainThread, @"the shared note is not on the main thread"); probe_announced++;
    }];
}
/* The main queue's blocks so far, run. */
static void probe_main_queue(void) {
    __block BOOL ran = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ ran = YES; });
    probe_wait(&ran, 10);
}

/* A JPEG's pixel size; {0, 0} when it is not one. */
static CGSize probe_jpeg_size(NSData *data) {
    const uint8_t *b = data.bytes;
    if (data.length < 3 || b[0] != 0xff || b[1] != 0xd8 || b[2] != 0xff) return CGSizeZero;
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    CGImageRef img = source ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
    CGSize size = img ? CGSizeMake(CGImageGetWidth(img), CGImageGetHeight(img)) : CGSizeZero;
    if (img) CGImageRelease(img);
    if (source) CFRelease(source);
    return size;
}
static CGSize probe_image_pixels(NSImage *img) {
    NSImageRep *rep = img.representations.firstObject;
    return rep ? CGSizeMake(rep.pixelsWide, rep.pixelsHigh) : CGSizeZero;
}

static NSDictionary *probe_up(CGImageRef frame) {
    /* (1) the switch off: nothing is kept or sent */
    expect(totk_shot_set(NO) && !totk_shot_on(), @"the switch did not turn off");
    expect(totk_shot_queue(frame) == 0, @"queued with the switch off");
    probe_drain();
    expect(probe_pending().count == 0, @"a picture was kept with the switch off");
    expect([probe_list()[@"shots"] count] == 0, @"the server has a picture although the switch was off");
    expect(totk_board_id(NO) == nil, @"a board id was made with the switch off");

    /* a body the server refuses for good (400) leaves the folder; the picture after it still goes */
    expect(totk_shot_set(YES) && totk_shot_on(), @"the switch did not turn on");
    NSString *dir = totk_board_file(@TOTK_SHOT_PENDING);
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    expect([[@"{\"schema\":1}" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[dir stringByAppendingPathComponent:@"1.json"] atomically:YES],
           @"the refused body was not written");

    /* (2) on: shared at 1920x1080 and marked as the player's own */
    expect(totk_shot_queue(frame) == 1, @"not queued with the switch on");
    probe_drain();
    /* (4) nothing waits once the server took it; the page heard of it once (not of the refused body) */
    expect(probe_pending().count == 0, [NSString stringWithFormat:@"pictures still wait: %@", probe_pending()]);
    probe_main_queue();
    expect(probe_announced == 1, [NSString stringWithFormat:@"the shared note came %d times, not once", probe_announced]);
    NSArray *shots = probe_list()[@"shots"];
    expect(shots.count == 1, [NSString stringWithFormat:@"the server lists %lu pictures, not 1", (unsigned long)shots.count]);
    NSDictionary *shot = shots.firstObject;
    expect([shot[@"me"] isEqual:@YES], @"the player's own picture is not marked me");
    expect([shot[@"w"] intValue] == 1920 && [shot[@"h"] intValue] == 1080, [NSString stringWithFormat:@"shared at %@x%@", shot[@"w"], shot[@"h"]]);
    NSString *shotId = shot[@"id"];

    NSURL *full = totk_shot_item_url(shotId, @"size", @"full");
    NSData *jpeg = [NSData dataWithContentsOfURL:full];
    CGSize size = probe_jpeg_size(jpeg);
    expect(size.width == 1920 && size.height == 1080, @"the full picture is not a 1920x1080 JPEG");
    expect(jpeg.length <= TOTK_SHOT_IMAGE_BYTES, @"the full picture is over the limit");

    __block NSImage *thumb = nil, *big = nil;
    __block BOOL came = NO;
    totk_shot_image(shotId, YES, ^(NSImage *img) { thumb = img; came = YES; });
    probe_wait(&came, 30);
    CGSize t = probe_image_pixels(thumb);
    expect(t.width == 400 && t.height == 225, [NSString stringWithFormat:@"the thumbnail is %.0fx%.0f", t.width, t.height]);
    expect([totk_shot_thumbs() objectForKey:shotId] == thumb, @"the thumbnail was not kept in memory");
    came = NO;
    totk_shot_image(shotId, NO, ^(NSImage *img) { big = img; came = YES; });
    probe_wait(&came, 30);
    expect(probe_image_pixels(big).width == 1920, @"the full picture was not read");

    /* (3) the player takes it back */
    __block BOOL deleted = NO;
    came = NO;
    totk_shot_delete(shotId, ^(BOOL ok) { deleted = ok; came = YES; });
    probe_wait(&came, 30);
    expect(deleted, @"the picture was not deleted");
    expect([probe_list()[@"shots"] count] == 0, @"the server still lists the deleted picture");
    expect([totk_shot_thumbs() objectForKey:shotId] == nil, @"the deleted picture's thumbnail is still kept");
    return @{@"shared": shotId, @"full_bytes": @(jpeg.length), @"announced": @(probe_announced)};
}

static NSDictionary *probe_down(CGImageRef frame) {
    expect(totk_shot_set(YES), @"the switch did not turn on");
    expect(totk_shot_queue(frame) == 1, @"not queued with the switch on");
    probe_drain();
    NSArray<NSString *> *files = probe_pending();
    expect(files.count == 1, [NSString stringWithFormat:@"%lu pictures wait, not 1", (unsigned long)files.count]);
    NSDictionary *body = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:files[0]] options:0 error:nil];
    expect([body[@"schema"] isEqual:@1] && [body[@"game"] isEqualToString:@GAME_INBOX_ID], @"the kept body has no schema or game");
    expect([body[@"board_id"] isEqualToString:totk_board_id(NO)], @"the kept body's id is not the board's");
    expect([body[@"width"] intValue] == 1920 && [body[@"height"] intValue] == 1080, @"the kept body is not 1920x1080");
    NSData *image = [[NSData alloc] initWithBase64EncodedString:body[@"image"] options:0];
    NSData *thumb = [[NSData alloc] initWithBase64EncodedString:body[@"thumb"] options:0];
    CGSize i = probe_jpeg_size(image), t = probe_jpeg_size(thumb);
    expect(i.width == 1920 && i.height == 1080 && image.length <= TOTK_SHOT_IMAGE_BYTES, @"the kept picture is not a 1920x1080 JPEG within the limit");
    expect(t.width == 400 && t.height == 225 && thumb.length <= TOTK_SHOT_THUMB_BYTES, @"the kept thumbnail is not a 400x225 JPEG within the limit");
    expect([[NSSet setWithArray:body.allKeys] isEqualToSet:[NSSet setWithArray:@[@"schema", @"game", @"board_id", @"image", @"thumb", @"width", @"height"]]],
           [NSString stringWithFormat:@"the kept body has other fields: %@", body.allKeys]);
    /* a second try without a server keeps it too */
    totk_shot_send_pending();
    probe_drain();
    expect(probe_pending().count == 1, @"the picture left the folder without a server");
    probe_main_queue();
    expect(probe_announced == 0, @"the shared note came although nothing was shared");
    return @{@"announced": @(probe_announced), @"pending": files[0].lastPathComponent, @"image_bytes": @(image.length), @"thumb_bytes": @(thumb.length),
             @"board_id": body[@"board_id"]};
}

static NSDictionary *probe_start(void) {
    expect(totk_shot_on() && probe_pending().count == 1, @"the down run left nothing waiting");
    totk_shot_start();
    totk_shot_start();   /* once per process */
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:30];
    while (probe_pending().count && end.timeIntervalSinceNow > 0) [NSThread sleepForTimeInterval:0.1];
    expect(probe_pending().count == 0, @"what waited was not sent after start");
    NSArray *shots = probe_list()[@"shots"];
    expect(shots.count == 1 && [shots[0][@"me"] isEqual:@YES], @"the server does not list the picture sent at start as the player's");
    return @{@"shared": shots[0][@"id"]};
}

static NSDictionary *probe_off(void) {
    expect(probe_pending().count == 1, @"nothing waits before the switch goes off");
    expect(totk_shot_set(NO), @"the switch did not turn off");
    expect(![NSFileManager.defaultManager fileExistsAtPath:totk_board_file(@TOTK_SHOT_PENDING)], @"what waited was kept after the switch went off");
    return @{};
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) fail(@"usage: shot_share_probe up|down|start|off");
        expect(totk_board_dir() != nil, @"TOTK_BOARD_DIR is not set");
        NSString *mode = @(argv[1]);
        probe_listen();
        CGImageRef frame = probe_frame();
        expect(CGImageGetWidth(frame) == 2560 && CGImageGetHeight(frame) == 1440, @"the probe's frame was not made");
        NSDictionary *result = [mode isEqualToString:@"up"] ? probe_up(frame) : [mode isEqualToString:@"down"] ? probe_down(frame)
                             : [mode isEqualToString:@"start"] ? probe_start() : [mode isEqualToString:@"off"] ? probe_off() : nil;
        CGImageRelease(frame);
        if (!result) fail(@"unknown mode");
        NSMutableDictionary *out = [result mutableCopy];
        out[@"status"] = @"pass";
        out[@"mode"] = mode;
        out[@"game"] = @GAME_INBOX_ID;
        printf("%s\n", [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:out options:0 error:nil] encoding:NSUTF8StringEncoding].UTF8String);
    }
    return 0;
}
