/* Finder entry point configuration for a locally packaged native host. */
#include "runtime.h"
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char rewritten[PATH_MAX];
static void directories(const char *path){
 char copy[PATH_MAX];if(strlen(path)>=sizeof copy)runtime_fail("Application data path too long");strcpy(copy,path);
 for(char *p=copy+1;;p++){if(*p=='/'||!*p){char saved=*p;*p=0;if(mkdir(copy,0700)&&errno!=EEXIST)runtime_fail("Create application data directory");*p=saved;if(!saved)break;}}
}
char *runtime_title_app_configure(void){
 char executable[PATH_MAX],contents[PATH_MAX],config[PATH_MAX],relative[PATH_MAX],candidate[PATH_MAX],work[PATH_MAX];
 uint32_t size=sizeof executable;if(_NSGetExecutablePath(executable,&size))runtime_fail("Executable path too long");
 if(!realpath(executable,contents))runtime_fail("Resolve application executable");
 char *end=strrchr(contents,'/');if(!end)runtime_fail("Application executable path");*end=0;
 end=strrchr(contents,'/');if(!end)runtime_fail("Application Contents path");*end=0;
 if(snprintf(config,sizeof config,"%s/Resources/host-path.txt",contents)>=(int)sizeof config)runtime_fail("Application configuration path");
 FILE *file=fopen(config,"r");if(!file)runtime_fail("Launch through a packaged app or use run.py with a host work directory");
 if(!fgets(relative,sizeof relative,file)){fclose(file);runtime_fail("Read application configuration");}fclose(file);relative[strcspn(relative,"\r\n")]=0;
 if(snprintf(candidate,sizeof candidate,"%s/Resources/%s",contents,relative)>=(int)sizeof candidate||!realpath(candidate,work))runtime_fail("Game data folder moved; repackage the app");
 if(chdir(work))runtime_fail("Open game data folder");
 if(snprintf(config,sizeof config,"%s/Resources/language.txt",contents)>=(int)sizeof config)runtime_fail("Language configuration path");
 file=fopen(config,"r");
 if(file){char language[16]={0};if(!fgets(language,sizeof language,file))runtime_fail("Empty language configuration");fclose(file);language[strcspn(language,"\r\n")]=0;if(strlen(language)>7)runtime_fail("Invalid language code");setenv("TOTK_LANG",language,1);}

 if(snprintf(rewritten,sizeof rewritten,"%s/rewritten",work)>=(int)sizeof rewritten)runtime_fail("Rewritten module path");
 const char *home=getenv("HOME");if(!home)runtime_fail("User home directory unavailable");
 char profile[PATH_MAX],romfs[PATH_MAX],slot[PATH_MAX],cache[PATH_MAX],logpath[PATH_MAX];
 if(snprintf(profile,sizeof profile,"%s/Library/Application Support/MacNativeGames/Tomodachi/profile",home)>=(int)sizeof profile)runtime_fail("Profile path");
 directories(profile);
 if(snprintf(config,sizeof config,"%s/Resources/romfs-path.txt",contents)>=(int)sizeof config)runtime_fail("RomFS configuration path");
 file=fopen(config,"r");if(!file)runtime_fail("RomFS configuration missing");
 if(!fgets(relative,sizeof relative,file)){fclose(file);runtime_fail("RomFS configuration empty");}fclose(file);relative[strcspn(relative,"\r\n")]=0;
 if(snprintf(candidate,sizeof candidate,"%s/Resources/%s",contents,relative)>=(int)sizeof candidate||!realpath(candidate,romfs))runtime_fail("Game RomFS missing");
 snprintf(slot,sizeof slot,"%s/../game-slot",profile);snprintf(cache,sizeof cache,"%s/build/shader-cache",work);
 setenv("TOTK_NATIVE_ROOT",work,1);setenv("TOTK_SHADER_SERVICE","native",1);
 setenv("TOTK_PROFILE_DIR",profile,1);setenv("TOTK_ROMFS",romfs,1);setenv("TOTK_ROMFS_PATCHES","0",1);
 setenv("TOTK_SHADER_CACHE",cache,1);setenv("TOTK_GAME_SLOT_DIR",slot,1);setenv("TOTK_SLOT_OWNER","user",1);
 /* Read/write attachment feedback must sample an immutable pre-draw image. */
 setenv("TOTK_ATT_SNAPSHOT","1",0);
 setenv("TOTK_GPU_BLOCKRT","1",1);setenv("TOTK_GPU_WIDEN","1",1);
 setenv("TOTK_GPU_ALLOW_SUBMIT","1",1);setenv("TOTK_GPU_MIDCLEAR","1",1);setenv("TOTK_ASYNC_REPLAY","0",1);setenv("TOTK_GPU_KEEPALIVE","0",1);
 setenv("TOTK_FPS_OVERLAY","0",1);setenv("TOTK_SHADER_OVERLAY","0",1);setenv("TOTK_LOAD_OVERLAY","0",1);
 unsetenv("TOTK_HEADLESS");unsetenv("TOTK_VERIFY_STARTUP");unsetenv("TOTK_INPUT_FILE");unsetenv("TOTK_AUTOLOAD");unsetenv("TOTK_ASSUME_FOCUS");
 snprintf(logpath,sizeof logpath,"%s/runtime.log",profile);const char *debug=getenv("TOMODACHI_DEBUG_LOG");int fd=debug&&strcmp(debug,"0")?open(logpath,O_WRONLY|O_CREAT|O_APPEND,0600):open("/dev/null",O_WRONLY);
 if(fd>=0){dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);fprintf(stderr,"\n[tomodachi] new app launch pid=%d\n",getpid());}
 extern void runtime_title_settings_prepare(void);runtime_title_settings_prepare();
 return rewritten;
}
