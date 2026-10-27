// Compiles the embedded pattern shader with the system's Metal compiler and checks its entry points.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>

#include "pattern_metal.h"

int main() {
    @autoreleasepool {
        // Command-line tools may get no default device; any device can compile the shader.
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) dev = MTLCopyAllDevices().firstObject;
        if (!dev) {
            std::printf("no Metal device\n");
            return 1;
        }
        NSError* e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kPatternMetal]
                                               options:[MTLCompileOptions new]
                                                 error:&e];
        if (!lib) {
            std::printf("Metal compile FAILED: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        for (NSString* fn in @[ @"vsMain", @"fsMain" ])
            if (![lib newFunctionWithName:fn]) {
                std::printf("missing entry point %s\n", fn.UTF8String);
                return 1;
            }
        std::printf("pattern.metal OK on %s\n", dev.name.UTF8String);
        return 0;
    }
}
