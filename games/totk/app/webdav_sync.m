/* WebDAV over NSURLSession (Foundation only, so the iPhone app builds it too): PROPFIND with Depth 1 per folder,
   GET, PUT, MKCOL, DELETE, Basic auth. One synchronous request at a time — a save root is a few files of a few MB
   (docs/save-sync-webdav.md), so the whole sync is seconds. */
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "save_sync.h"
#include "webdav_sync.h"

static void dav_log(const char *fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    fprintf(stderr, "[save-sync] %s\n", line);
}

static void dav_log_line(void *ud, const char *line) { (void)ud; dav_log("%s", line); }

static NSString *setting(NSString *key) {
    const char *e = getenv(key.UTF8String);
    if (e && *e) return @(e);
    return [NSUserDefaults.standardUserDefaults stringForKey:key];
}

mng_webdav_config mng_webdav_config_read(void) {
    static char url[1024], user[256], password[256];
    mng_webdav_config c = {NULL, NULL, NULL};
    NSString *u = setting(@"TOTK_WEBDAV_URL");
    if (![u isKindOfClass:NSString.class]) return c;
    u = [u stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (![u.lowercaseString hasPrefix:@"http://"] && ![u.lowercaseString hasPrefix:@"https://"]) return c;   // off, or a typo to say no to
    while ([u hasSuffix:@"/"]) u = [u substringToIndex:u.length - 1];
    snprintf(url, sizeof url, "%s", u.UTF8String);
    c.url = url;
    NSString *us = setting(@"TOTK_WEBDAV_USER"), *pw = setting(@"TOTK_WEBDAV_PASSWORD");
    if ([us isKindOfClass:NSString.class] && us.length) { snprintf(user, sizeof user, "%s", us.UTF8String); c.user = user; }
    if ([pw isKindOfClass:NSString.class] && pw.length) { snprintf(password, sizeof password, "%s", pw.UTF8String); c.password = password; }
    return c;
}

typedef struct {
    NSURL *root;                // .../MacNativeGames/<game>/saves
    NSString *auth;             // the Authorization header value, or nil
    NSURLSession *session;
} Dav;

/* One request, waited for: the launcher is synchronous by nature (it execv's the game when done). */
static NSData *dav_op(Dav *d, NSString *method, NSURL *url, NSData *body, NSDictionary<NSString *, NSString *> *headers,
                      NSString *fromFile, long long *outStatus) {
    NSMutableURLRequest *rq = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:60];
    rq.HTTPMethod = method;
    rq.HTTPBody = body;
    if (d->auth) [rq setValue:d->auth forHTTPHeaderField:@"Authorization"];
    for (NSString *k in headers) [rq setValue:headers[k] forHTTPHeaderField:k];
    __block NSData *data = nil;
    __block NSHTTPURLResponse *response = nil;
    __block NSError *error = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionTask *task = fromFile
        ? [d->session uploadTaskWithRequest:rq fromFile:[NSURL fileURLWithPath:fromFile]
                          completionHandler:^(NSData *got, NSURLResponse *r, NSError *e) { data = got; response = (NSHTTPURLResponse *)r; error = e; dispatch_semaphore_signal(done); }]
        : [d->session dataTaskWithRequest:rq
                        completionHandler:^(NSData *got, NSURLResponse *r, NSError *e) { data = got; response = (NSHTTPURLResponse *)r; error = e; dispatch_semaphore_signal(done); }];
    [task resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (outStatus) *outStatus = response.statusCode;
    if (error) dav_log("%s %s: %s", method.UTF8String, url.absoluteString.UTF8String, error.localizedDescription.UTF8String);
    return error ? nil : data;
}

static BOOL dav_status_ok(long long status, NSArray<NSNumber *> *good, BOOL missingFine) {
    if (missingFine && status == 404) return YES;
    for (NSNumber *s in good) if (s.longLongValue == status) return YES;
    return NO;
}

static NSURL *dav_url(Dav *d, const char *rel) {
    return [d->root URLByAppendingPathComponent:@(rel)];
}

static NSString *local_name(NSString *element) {   // "D:response" -> "response": servers prefix their namespaces
    NSRange colon = [element rangeOfString:@":"];
    return colon.location == NSNotFound ? element : [element substringFromIndex:colon.location + 1];
}

/* PROPFIND's multistatus: the href of every entry and whether it is a folder. Namespace by local name only. */
@interface DavProps : NSObject <NSXMLParserDelegate>
@property (nonatomic) NSMutableArray<NSString *> *hrefs;
@property (nonatomic) NSMutableArray<NSNumber *> *collections;
@property (nonatomic) NSString *href;
@property (nonatomic) BOOL collection, inHref, inResponse;
@end
@implementation DavProps
- (instancetype)init {
    if ((self = [super init])) { _hrefs = [NSMutableArray array]; _collections = [NSMutableArray array]; }
    return self;
}
- (void)parser:(NSXMLParser *)p didStartElement:(NSString *)e namespaceURI:(NSString *)u qualifiedName:(NSString *)q attributes:(NSDictionary *)a {
    (void)p; (void)u; (void)q; (void)a;
    NSString *name = local_name(e);
    if ([name isEqualToString:@"response"]) { self.href = nil; self.collection = NO; self.inResponse = YES; }
    else if ([name isEqualToString:@"collection"]) self.collection = YES;
    else if ([name isEqualToString:@"href"]) self.inHref = YES;
}
- (void)parser:(NSXMLParser *)p foundCharacters:(NSString *)s {
    (void)p;
    if (self.inHref) self.href = self.href ? [self.href stringByAppendingString:s] : s;
}
- (void)parser:(NSXMLParser *)p didEndElement:(NSString *)e namespaceURI:(NSString *)u qualifiedName:(NSString *)q {
    (void)p; (void)u; (void)q;
    NSString *name = local_name(e);
    if ([name isEqualToString:@"response"]) {
        self.inResponse = NO;
        if (self.href) {
            [self.hrefs addObject:self.href];
            [self.collections addObject:@(self.collection)];
        }
        self.href = nil;
    } else if ([name isEqualToString:@"href"]) self.inHref = NO;
}
@end

/* The hrefs come percent-encoded, absolute or relative, with or without a trailing slash: make one relative path of them. */
static NSString *dav_rel(NSString *href, NSString *basePath) {
    NSString *path = href;
    if ([path hasPrefix:@"http"]) path = [NSURL URLWithString:path].path ?: path;
    path = [path stringByRemovingPercentEncoding] ?: path;
    if (![path hasPrefix:basePath]) return nil;
    path = [path substringFromIndex:basePath.length];
    while ([path hasPrefix:@"/"]) path = [path substringFromIndex:1];
    while ([path hasSuffix:@"/"]) path = [path substringToIndex:path.length - 1];
    return path.length ? path : nil;    // nil: the listed folder itself
}

static NSString * const XML_PAYLOAD = @"<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/></D:prop></D:propfind>";

static int dav_list_dir(Dav *d, NSURL *dir, NSString *basePath, NSString *prefix, NSMutableArray<NSString *> *rels,
                        NSMutableArray<NSNumber *> *sizes, int depth) {
    if (depth > 8) return -1;
    NSData *propfind = [XML_PAYLOAD dataUsingEncoding:NSUTF8StringEncoding];
    long long status = 0;
    NSData *xml = dav_op(d, @"PROPFIND", dir, propfind, @{ @"Depth": @"1", @"Content-Type": @"application/xml" }, nil, &status);
    if (status == 404) return 0;            // a remote root not made yet is a fine first sync
    if (!xml || status != 207) return -1;
    DavProps *props = [DavProps new];
    NSXMLParser *parser = [[NSXMLParser alloc] initWithData:xml];
    parser.delegate = props;
    if (![parser parse]) return -1;
    for (NSUInteger i = 0; i < props.hrefs.count; i++) {
        NSString *rel = dav_rel(props.hrefs[i], basePath);
        if (!rel || [rel isEqualToString:@(MNG_SYNC_STATE_NAME)]) continue;
        if (props.collections[i].boolValue) {
            NSString *name = [rel lastPathComponent];
            if (dav_list_dir(d, [dir URLByAppendingPathComponent:name], [basePath stringByAppendingPathComponent:name],
                             [prefix stringByAppendingFormat:@"%@/", rel], rels, sizes, depth + 1)) return -1;
        } else {
            [rels addObject:[prefix stringByAppendingString:rel]];
            [sizes addObject:@0];           // the engine hashes what it downloads; the listing's sizes go unused
        }
    }
    return 0;
}

static int dav_list(void *ctx, char ***out_files, long long **out_sizes, int *out_count) {
    Dav *d = ctx;
    NSMutableArray<NSString *> *rels = [NSMutableArray array];
    NSMutableArray<NSNumber *> *sizes = [NSMutableArray array];
    if (dav_list_dir(d, d->root, d->root.path ?: @"", @"", rels, sizes, 0)) return -1;
    int n = (int)rels.count;
    char **files = malloc(sizeof(char *) * (size_t)(n ? n : 1));
    long long *got = malloc(sizeof(long long) * (size_t)(n ? n : 1));
    if (!files || !got) { free(files); free(got); return -1; }
    for (int i = 0; i < n; i++) {
        files[i] = strdup(rels[i].UTF8String);
        got[i] = sizes[i].longLongValue;
    }
    *out_files = files;
    *out_sizes = got;
    *out_count = n;
    return 0;
}

static int dav_fetch(void *ctx, const char *rel, const char *to) {
    Dav *d = ctx;
    long long status = 0;
    NSData *data = dav_op(d, @"GET", dav_url(d, rel), nil, @{ }, nil, &status);
    if (!data || status != 200) return -1;
    return [NSFileManager.defaultManager createFileAtPath:@(to) contents:data attributes:nil] ? 0 : -1;
}

static int dav_mkcol(Dav *d, NSURL *dir) {
    long long status = 0;
    dav_op(d, @"MKCOL", dir, nil, @{ }, nil, &status);
    return dav_status_ok(status, @[@201, @200, @301, @405], YES) ? 0 : -1;   // 405: it is there already
}

static int dav_store(void *ctx, const char *rel, const char *from) {
    Dav *d = ctx;
    /* The folders of the path, in order: some servers refuse a PUT whose collection is not there yet. */
    NSArray<NSString *> *parts = [@(rel) pathComponents];
    NSMutableString *made = [d->root.absoluteString mutableCopy];
    for (NSUInteger i = 1; i + 1 < parts.count; i++) {   // parts[0] is "/"
        [made appendFormat:@"/%@", parts[i]];
        if (dav_mkcol(d, [NSURL URLWithString:made])) return -1;
    }
    long long status = 0;
    dav_op(d, @"PUT", dav_url(d, rel), nil, @{ @"Content-Type": @"application/octet-stream" }, @(from), &status);
    return dav_status_ok(status, @[@200, @201, @204], NO) ? 0 : -1;
}

static int dav_remove(void *ctx, const char *rel) {
    Dav *d = ctx;
    long long status = 0;
    dav_op(d, @"DELETE", dav_url(d, rel), nil, @{ }, nil, &status);
    return dav_status_ok(status, @[@200, @202, @204], YES) ? 0 : -1;
}

static void dav_release(void *ctx, char **files, long long *sizes, int count) {
    (void)ctx;
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    free(sizes);
}

/* The save root of this profile's account (profile/save/<16 hex of the account, high half first>), as fs_host.c and
   scripts/save_manager.py name it. Nil when the account identity is not there yet (the game makes it at its first
   start); the sync then has nothing of its own to move and waits. */
static NSString *account_save_dir(NSString *profile) {
    NSString *save = [profile stringByAppendingPathComponent:@"save"];
    NSArray<NSString *> *entries = [NSFileManager.defaultManager contentsOfDirectoryAtPath:save error:nil] ?: @[];
    for (NSString *name in entries) {
        if ([name hasPrefix:@"."] || name.length != 32) continue;
        BOOL isDir = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:[save stringByAppendingPathComponent:name] isDirectory:&isDir] && isDir)
            return [save stringByAppendingPathComponent:name];
    }
    NSData *uid = [NSData dataWithContentsOfFile:[profile stringByAppendingPathComponent:@"account.uid"]];
    if (uid.length != 16) return nil;
    uint64_t lo = 0, hi = 0;
    const unsigned char *b = uid.bytes;
    for (int i = 7; i >= 0; i--) { lo = lo << 8 | b[i]; hi = hi << 8 | b[8 + i]; }
    if (!lo && !hi) return nil;
    return [save stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx%016llx", hi, lo]];
}

void mng_webdav_sync_profile(const char *game_folder, const char *profile, const char *slot_dir) {
    @autoreleasepool {
        mng_webdav_config cfg = mng_webdav_config_read();
        if (!cfg.url || !*cfg.url) return;
        CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        /* A game running on this Mac holds the slot lock (runtime/game_slot.c); the launcher checked already, and this
           is the same test again for any other caller. The iPhone passes NULL: there the game runs in this process, and
           the caller checks runtime_fs_save_busy() instead. */
        if (slot_dir && *slot_dir) {
            int fd = open([[NSString stringWithFormat:@"%s/lock", slot_dir] fileSystemRepresentation], O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
                close(fd);
                if (held) { dav_log("the game is running; syncing another time"); return; }
            }
        }
        NSString *saveDir = account_save_dir(@(profile));
        if (!saveDir) { dav_log("no account identity yet; the game makes one at its first start"); return; }
        NSURL *base = [NSURL URLWithString:@(cfg.url)];
        if (!base) { dav_log("%s is not a URL", cfg.url); return; }
        Dav d = {0};
        d.root = base;
        NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = 60;
        d.session = [NSURLSession sessionWithConfiguration:config];
        if (cfg.user && *cfg.user) {
            NSData *pair = [[NSString stringWithFormat:@"%s:%s", cfg.user, cfg.password ?: ""] dataUsingEncoding:NSUTF8StringEncoding];
            d.auth = [@"Basic " stringByAppendingString:[pair base64EncodedStringWithOptions:0]];
        }
        for (NSString *part in @[@"MacNativeGames", @(game_folder), @"saves"]) {
            d.root = [d.root URLByAppendingPathComponent:part];
            if (dav_mkcol(&d, d.root)) { dav_log("the folder on the server could not be made"); return; }
        }
        NSString *staging = [@(profile) stringByAppendingPathComponent:@"sync-staging"];
        [NSFileManager.defaultManager removeItemAtPath:staging error:nil];
        mng_sync_backend be = {&d, dav_list, dav_fetch, dav_store, dav_remove, dav_release};
        int got = 0, sent = 0, deleted = 0, conflicts = 0;
        /* The iPhone can call this again from applicationDidEnterBackground while an earlier sync is still running
           (termination): the staging folder and the manifests are per profile, so the runs must not overlap. */
        static NSLock *running;
        static dispatch_once_t once;
        dispatch_once(&once, ^{ running = [NSLock new]; });
        [running lock];
        int failed = mng_save_sync(saveDir.fileSystemRepresentation,
                                   [@(profile) stringByAppendingPathComponent:@"sync-manifest.txt"].fileSystemRepresentation,
                                   staging.fileSystemRepresentation,
                                   [@(profile) stringByAppendingPathComponent:@"sync-backups"].fileSystemRepresentation,
                                   &be,
                                   dav_log_line, NULL,
                                   &got, &sent, &deleted, &conflicts);
        [running unlock];
        dav_log("%s in %.1f s (%d taken, %d sent, %d deleted, %d conflicts)",
                failed ? "did not finish" : "finished", CFAbsoluteTimeGetCurrent() - t0, got, sent, deleted, conflicts);
    }
}
