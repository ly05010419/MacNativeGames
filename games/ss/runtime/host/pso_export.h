// A player's shader list contribution: the rows of their pso_user.tsv (shader libraries, pipelines this Mac built while playing,
// written by nvn_mtl.m pso_user_record) that the app's bundled warm-up seed does not hold, exported to a file they choose and
// send by hand. Nothing is sent and nothing is marked: the user list stays as it is. Keys only, no game data.
// Rows are checked as scripts/pso_seed.py checks them (S <key> <stage 0-5>, C <key>, R <pso_skey as lowercase hex>, a key being
// 16 lowercase hex digits and "_b"); a row cut short at the end of the file (the game may be writing it) is left out and named,
// any other bad row stops the export with its line. AppKit-free, so the native UI test drives it with its own files.
#pragma once
#import <Foundation/Foundation.h>
// The pso_skey layout (runtime/gfx/nvn/nvn_pso_skey.h): tests/test_pso_seed.py checks these against the header itself.
enum { DM_PSO_SKEY_SIZE = 440, DM_PSO_VS = 0, DM_PSO_FS = 24, DM_PSO_KEYLEN = 24, DM_PSO_VS_STAGE = 48, DM_PSO_FS_STAGE = 49, DM_PSO_MERGED = 50, DM_PSO_FZ = 51 };
NSString *pso_user_path(void);   // runtime/gfx/nvn/nvn_mtl.m: DM_PSO_USER, or pso_user.tsv beside the save folder (DM_SAVE)
@interface DmPsoExport : NSObject
@property (readonly) NSUInteger rows;          // valid, unique rows the seed lacks: what the file holds
@property (readonly) BOOL partialTail;         // the last line had no newline yet and was left out
@property (readonly, copy) NSString *error;    // nil, or why nothing can be exported ("line 12: ...")
@property (readonly, copy) NSString *body;     // the rows, one per line (nil on error)
// user: pso_user.tsv's bytes as read once (nil: no file yet, no rows); seed: the bundled pso_manifest.tsv (nil: none).
+ (instancetype)fromUser:(NSData *)user seed:(NSString *)seed;
+ (instancetype)fromUserPath:(NSString *)user seedPath:(NSString *)seed;   // reads each file once
// Writes the header and the rows to url, atomically (a temporary file renamed over it): nil on success, else what went wrong.
// Never writes when error is set or when there are no rows.
- (NSString *)writeTo:(NSURL *)url date:(NSDate *)date;
@end
