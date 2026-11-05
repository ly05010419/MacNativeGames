/* Local-only, per-profile installation identity. No hardware identifiers or
   Nintendo account/device services are queried. The existing random account
   UID is persisted by account.c; a namespace hash separates its device role. */
#include "runtime.h"
#include "fs_host.h"
#include <CommonCrypto/CommonDigest.h>
#include <string.h>

void runtime_pseudo_device_id(uint8_t *out) {
    if (!out) runtime_fail("pseudo device UUID output");
    struct { char domain[40]; uint64_t uid[2]; } input = {"MacNativeGames.XCX.PseudoDevice.v1", {0, 0}};
    runtime_account_uid(input.uid);
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    if (!CC_SHA256(&input, sizeof input, digest)) runtime_fail("pseudo device UUID digest");
    /* RFC 9562 custom UUID (version 8), represented by the SDK's 16 raw bytes. */
    digest[6] = (digest[6] & 15) | 0x80;
    digest[8] = (digest[8] & 63) | 0x80;
    memcpy(out, digest, 16);
}
