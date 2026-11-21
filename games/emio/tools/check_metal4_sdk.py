"""Check the selected Apple SDK before producing Metal 4 build artifacts."""
import subprocess
import sys


def main():
    version = subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-version"], text=True).strip()
    if int(version.split(".")[0]) < 26:
        sys.exit(f"Metal 4 requires the macOS 26 or newer SDK; selected SDK is {version}. "
                 "Update Xcode or select a newer installation with DEVELOPER_DIR.")
    source = '#import <Metal/Metal.h>\nvoid check(void) { MTL4CommandQueueDescriptor *q = [MTL4CommandQueueDescriptor new]; MTL4RenderPassDescriptor *r = [MTL4RenderPassDescriptor new]; (void)q; (void)r; }\n'
    subprocess.run(["xcrun", "clang", "-x", "objective-c", "-fobjc-arc",
                    "-mmacosx-version-min=26.0", "-Werror", "-fsyntax-only", "-"],
                   input=source, text=True, check=True)


if __name__ == "__main__":
    main()
