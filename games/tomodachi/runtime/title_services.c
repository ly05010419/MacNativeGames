/* Tomodachi service bindings. Match each SDK ABI; do not return fake success
   for unimplemented Mii, network or save operations. */
#include "runtime.h"
#include <string.h>
#include <stdatomic.h>
#include <CoreFoundation/CoreFoundation.h>
#include "title_config.h"
extern void runtime_account_uid(uint64_t *uid);
static unsigned save_data_size(int64_t *size,int64_t *journal,const uint64_t *uid) {
    uint64_t local[2];runtime_account_uid(local);
    if(!size||!journal||!uid)return 0x2ee202;
    if(uid[0]!=local[0]||uid[1]!=local[1])return 0x7d402;
    *size=TOMODACHI_SAVE_SIZE;*journal=TOMODACHI_JOURNAL_SIZE;return 0;
}
static unsigned save_data_size_max(int64_t *size,int64_t *journal) {
    if(!size||!journal)return 0x2ee202;
    *size=TOMODACHI_SAVE_SIZE_MAX;*journal=TOMODACHI_JOURNAL_SIZE_MAX;return 0;
}
static _Atomic unsigned exit_sections;
int runtime_title_exit_busy(void) { return atomic_load(&exit_sections)!=0; }
static _Atomic int album_notifications;
/* macOS screenshots do not generate Horizon album events. Retain the
   subscription, but never synthesize a successful capture notification. */
static void enable_album_notifications(void) { atomic_store(&album_notifications,1); }
static int region_code(void) {
    CFLocaleRef locale=CFLocaleCopyCurrent();
    CFStringRef country=CFLocaleGetValue(locale,kCFLocaleCountryCode);
    int region=1;
    if(country){
        if(CFEqual(country,CFSTR("JP")))region=0;
        else if(CFEqual(country,CFSTR("AU"))||CFEqual(country,CFSTR("NZ")))region=3;
        else if(CFEqual(country,CFSTR("CN")))region=4;
        else if(CFEqual(country,CFSTR("KR")))region=5;
        else if(CFEqual(country,CFSTR("TW"))||CFEqual(country,CFSTR("HK")))region=6;
    }
    CFRelease(locale);return region;
}
static void enter_exit_section(void) { atomic_fetch_add(&exit_sections,1); }
static void leave_exit_section(void) {
    unsigned count=atomic_load(&exit_sections);
    do {
        if(!count)runtime_fail("Unbalanced application exit section");
    } while(!atomic_compare_exchange_weak(&exit_sections,&count,count-1));
}
static void display_version(void *output) {
    if(!output)runtime_fail("GetDisplayVersion: null output");
    memset(output,0,16);
    memcpy(output,"1.0.0",5);
}
void *runtime_title_override(const char *name) {
    extern void *runtime_title_input_override(const char *);
    void *input=runtime_title_input_override(name);if(input)return input;
    extern void *runtime_title_keyboard_override(const char *);
    void *keyboard=runtime_title_keyboard_override(name);if(keyboard)return keyboard;
    extern void *runtime_title_filter_override(const char *);
    void *filter=runtime_title_filter_override(name);if(filter)return filter;
    if(!strcmp(name,"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE"))return display_version;
    if(!strcmp(name,"_ZN2nn2oe31EnterExitRequestHandlingSectionEv"))return enter_exit_section;
    if(!strcmp(name,"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv"))return leave_exit_section;
    if(!strcmp(name,"_ZN2nn2oe33EnableAlbumImageTakenNotificationEv"))return enable_album_notifications;
    if(!strcmp(name,"_ZN2nn2oe13GetRegionCodeEv"))return region_code;
    if(!strcmp(name,"_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE"))return save_data_size;
    if(!strcmp(name,"_ZN2nn2fs18GetSaveDataSizeMaxEPlS1_"))return save_data_size_max;
    return NULL;
}
