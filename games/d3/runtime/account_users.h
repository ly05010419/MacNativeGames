/* Offline profile users: stable numeric IDs, mutable UTF-8 labels.
   Shared text format with the launchers: selected<TAB>id, then id<TAB>name.
   The chosen identity is frozen for the process lifetime; no live save switching. */
#ifndef MNG_ACCOUNT_USERS_H
#define MNG_ACCOUNT_USERS_H
#include <limits.h>
#define MNG_MAX_USERS 32
static unsigned account_user_ids[MNG_MAX_USERS],account_user_count,account_selected;
static char account_user_names[MNG_MAX_USERS][33];
static int account_user_position(unsigned id) {
 for(unsigned i=0;i<account_user_count;i++)if(account_user_ids[i]==id)return (int)i;
 return -1;
}
static void account_users_load(const char *dir) {
 char path[PATH_MAX];
 int length=snprintf(path,sizeof path,"%s/users.tsv",dir);
 if(length<0||(size_t)length>=sizeof path)runtime_fail("local user registry path too long");
 int fd=open(path,O_RDONLY|O_NOFOLLOW);struct stat st;
 if(fd<0){
  if(errno!=ENOENT)runtime_fail("read local user registry");
  account_user_count=1;account_user_ids[0]=1;account_selected=1;
  strcpy(account_user_names[0],"User 01");

 }else{
  if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>4096)runtime_fail("invalid local user registry");
  FILE *f=fdopen(fd,"r");if(!f)runtime_fail("open local user registry");
  char line[256];unsigned selected=0;char tail;
  if(!fgets(line,sizeof line,f)||sscanf(line,"selected\t%u %c",&selected,&tail)!=1)runtime_fail("invalid selected user");
  while(fgets(line,sizeof line,f)){
   char *tab=strchr(line,'\t'),*end=NULL;if(!tab||!strchr(line,'\n'))runtime_fail("invalid local user row");
   *tab++=0;unsigned long id=strtoul(line,&end,10);size_t n=strcspn(tab,"\n");tab[n]=0;
   if(*end||!id||id>MNG_MAX_USERS||!n||n>32||strchr(tab,'\t')||strchr(tab,'\r')||account_user_count>=MNG_MAX_USERS||account_user_position((unsigned)id)>=0)
    runtime_fail("invalid local user identity or name");
   unsigned i=account_user_count++;account_user_ids[i]=(unsigned)id;memcpy(account_user_names[i],tab,n+1);
  }
  if(ferror(f))runtime_fail("read local user registry");fclose(f);
  if(!account_user_count||account_user_position(selected)<0)runtime_fail("selected local user does not exist");
  account_selected=selected;
 }
 const char *e=getenv("MNG_USER_ID");
 if(e&&*e){char *end;unsigned long id=strtoul(e,&end,10);
  if(*end||!id||id>MNG_MAX_USERS||account_user_position((unsigned)id)<0)runtime_fail("requested local user does not exist");
  account_selected=(unsigned)id;
 }
}
static const char *account_user_label(unsigned id) {
 int i=account_user_position(id);return i<0?"":account_user_names[i];
}
#endif
