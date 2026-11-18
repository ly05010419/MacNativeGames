// Builds one mesh render pipeline (object + mesh + fragment library) the way the pipeline archive does (nvn_mtl.m par_write:
// addMeshRenderPipelineFunctionsWithDescriptor on an MTLBinaryArchive), on this Mac's own Metal compiler service, and prints
// one JSON line: {"ok": true|false, "seconds": s, "error": "..."}. Runs no game code. Used by reduce.py (#2107).
// usage: mesh_probe <object.metallib|-> <mesh.metallib> <fragment.metallib> [mesh entry]
#import <Metal/Metal.h>

static void report(BOOL ok, double seconds, NSString *error) {
    NSDictionary *d = @{@"ok": @(ok), @"seconds": @(seconds), @"error": error ?: @""};
    NSData *j = [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
    printf("%s\n", [[NSString alloc] initWithData:j encoding:NSUTF8StringEncoding].UTF8String);
}

static id<MTLFunction> function(id<MTLDevice> dev, const char *path, NSString *name, NSString **error) {
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&e];
    if (!lib) { *error = [NSString stringWithFormat:@"library %s: %@", path, e.localizedDescription]; return nil; }
    id<MTLFunction> f = [lib newFunctionWithName:name];
    if (!f) *error = [NSString stringWithFormat:@"no function %@ in %s", name, path];
    return f;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 4) { fprintf(stderr, "usage: mesh_probe <object.metallib|-> <mesh.metallib> <fragment.metallib> [mesh entry]\n"); return 2; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSString *error = nil;
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
    if (strcmp(argv[1], "-") && !(d.objectFunction = function(dev, argv[1], @"main0", &error))) { report(NO, 0, error); return 0; }
    if (!(d.meshFunction = function(dev, argv[2], argc > 4 ? @(argv[4]) : @"main0", &error))) { report(NO, 0, error); return 0; }
    if (!(d.fragmentFunction = function(dev, argv[3], @"main0", &error))) { report(NO, 0, error); return 0; }
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;   // both failing pipelines write one float4 colour
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *e = nil;
    id<MTLBinaryArchive> archive = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
    if (!archive) { report(NO, 0, [NSString stringWithFormat:@"archive: %@", e.localizedDescription]); return 0; }
    NSDate *t = [NSDate date];
    BOOL ok = [archive addMeshRenderPipelineFunctionsWithDescriptor:d error:&e];
    report(ok, -t.timeIntervalSinceNow, ok ? nil : (e.localizedDescription ?: @"unknown error"));
    return 0;
}}
