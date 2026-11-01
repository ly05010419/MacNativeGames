#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *runtime_time_override(const char *);
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"%s\n",message);abort();}
typedef struct {int16_t year;int8_t month,day,hour,minute,second,pad;} Calendar;
int main(void){
 unsigned(*load)(void*,const char*)=runtime_time_override("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE");
 unsigned(*to_local)(Calendar*,void*,const int64_t*,const void*)=runtime_time_override("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE");
 unsigned(*to_posix)(int*,int64_t*,int,const Calendar*,const void*)=runtime_time_override("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE");
 char rule[0x4000],zone[0x24]="America/Los_Angeles";assert(load(rule,zone)==0);
 int64_t epoch=0;Calendar c;assert(to_local(&c,NULL,&epoch,rule)==0);
 assert(c.year==1969&&c.month==12&&c.day==31&&c.hour==16);
 int count;int64_t times[2];assert(to_posix(&count,times,2,&c,rule)==0);assert(count==1&&times[0]==0);
 Calendar spring={2026,3,8,2,30,0,0};assert(to_posix(&count,times,2,&spring,rule)==0);assert(count==0);
 Calendar autumn={2026,11,1,1,30,0,0};assert(to_posix(&count,times,2,&autumn,rule)==0);assert(count==2);
 assert(llabs(times[0]-times[1])==3600);
 for(int i=0;i<count;i++){assert(to_local(&c,NULL,&times[i],rule)==0);assert(!memcmp(&c,&autumn,7));}
 memset(rule,0,sizeof rule);assert(to_local(&c,NULL,&epoch,rule)!=0);
 puts("Native clock timezone, epoch round-trip, DST gap and DST fold tests passed");
}
