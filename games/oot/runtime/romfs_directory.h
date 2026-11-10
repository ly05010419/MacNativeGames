/* Native directory enumeration from the verified packed filesystem metadata.
 * Snapshots own their cursor; counts never consume entries. Overlays contribute
 * added paths and override sizes, using the same ownership as file reads. */
static char *rom_directory_text;
static char **rom_directory_names;
static size_t rom_directory_count;
static int directory_record_exists(const char *name) {
 size_t n=strlen(name);while(n&&name[n-1]=='/')n--;if(!n)return 1;
 for(size_t i=0;i<rom_directory_count;i++)if(strlen(rom_directory_names[i])==n&&!memcmp(name,rom_directory_names[i],n))return 1;
 return 0;
}
static void rom_directory_metadata_init(const char *dir) {
 char path[4096];need(snprintf(path,sizeof path,"%s/runtime-directories.txt",dir)<(int)sizeof path,"directory metadata path");
 int fd=open(path,O_RDONLY);need(fd>=0,"verified directory metadata missing");struct stat st;need(!fstat(fd,&st)&&st.st_size>0&&st.st_size<(4<<20),"directory metadata bounds");
 sha_fd(fd,runtime_game_sha256("romfs-config.txt","ROMFS_DIRECTORIES_SHA256"),0,NULL);
 size_t size=(size_t)st.st_size;rom_directory_text=malloc(size+1);need(rom_directory_text!=NULL,"directory metadata allocation");
 need(pread(fd,rom_directory_text,size,0)==(ssize_t)size,"directory metadata read");close(fd);rom_directory_text[size]=0;
 need(rom_directory_text[size-1]=='\n',"directory metadata terminator");
 char *previous=NULL;for(char *p=rom_directory_text;*p;){char *end=strchr(p,'\n');need(end!=NULL,"directory metadata line");*end=0;
  need(path_ok(p)&&(!previous||strcmp(previous,p)<0),"sorted valid directory metadata");
  char **names=realloc(rom_directory_names,(rom_directory_count+1)*sizeof *names);need(names!=NULL,"directory metadata names");rom_directory_names=names;rom_directory_names[rom_directory_count++]=p;previous=p;p=end+1;
 }
}
#define ROM_DIR_BASE 0x400000000ULL
#define ROM_DIR_MAX 64
static struct { int used;RuntimeDirectoryEntry *entries;size_t count,pos; } rom_dirs[ROM_DIR_MAX];
static int rom_child_order(const void *a,const void *b){return strcmp(((const RuntimeDirectoryEntry *)a)->name,((const RuntimeDirectoryEntry *)b)->name);}
static void rom_child_add(RuntimeDirectoryEntry **list,size_t *count,const char *path,const char *prefix,int is_dir,uint64_t size,unsigned mode){
 size_t plen=strlen(prefix);if(strncmp(path,prefix,plen))return;
 const char *name=path+plen;if(!*name)return;
 const char *slash=strchr(name,'/');size_t len=slash?(size_t)(slash-name):strlen(name);is_dir|=slash!=NULL;
 if(!(mode&(is_dir?1:2)))return;
 need(len>0&&len<=768,"directory child name bounds");
 RuntimeDirectoryEntry *p=realloc(*list,(*count+1)*sizeof *p);need(p!=NULL,"directory snapshot allocation");*list=p;
 RuntimeDirectoryEntry *e=&p[(*count)++];memset(e,0,sizeof *e);memcpy(e->name,name,len);e->type=is_dir?0:1;e->size=is_dir||(mode&0x80000000u)?0:(int64_t)size;
}
int runtime_romfs_directory_handle(uint64_t h){return h>=ROM_DIR_BASE&&h<ROM_DIR_BASE+ROM_DIR_MAX;}
unsigned runtime_romfs_directory_open(uint64_t *out,const char *path,int mode){
 unsigned flags=(unsigned)mode;if(!out||!path||!(flags&3)||(flags&~0x80000003u))return INVALID;
 runtime_fs_init();const char *relative=rom_path(path);char prefix[4096];
 const char *colon=strchr(path,':');const char *root=colon?colon+1:"";int is_root=runtime_romfs_owns(path)&&*root=='/'&&strspn(root,"/")==strlen(root);
 if(!relative&&!is_root)return NOT_FOUND;
 if(is_root)prefix[0]=0;else{size_t n=strlen(relative);while(n&&relative[n-1]=='/')n--;if(n+2>sizeof prefix)return INVALID;memcpy(prefix,relative,n);prefix[n]='/';prefix[n+1]=0;}
 if(*prefix&&!folder_exists(prefix))return NOT_FOUND;
 RuntimeDirectoryEntry *list=NULL;size_t count=0,plen=strlen(prefix);
 for(uint64_t i=lower(prefix);i<file_count;i++){
  Entry e=entry(i);const char *name=entry_name(e);if(strncmp(name,prefix,plen))break;
  uint32_t ov=overlay_slot(i);rom_child_add(&list,&count,name,prefix,0,ov?overlays[ov-1].size:e.size,flags);
 }
 for(size_t i=0;i<noverlay;i++)if(overlays[i].index>=file_count)rom_child_add(&list,&count,overlays[i].name,prefix,0,overlays[i].size,flags);
 for(size_t i=0;i<rom_directory_count;i++)rom_child_add(&list,&count,rom_directory_names[i],prefix,1,0,flags);
 if(count>1)qsort(list,count,sizeof *list,rom_child_order);
 size_t unique=0;for(size_t i=0;i<count;i++){if(unique&&!strcmp(list[i].name,list[unique-1].name)){need(list[i].type==list[unique-1].type,"file/directory path collision");continue;}list[unique++]=list[i];}
 pthread_mutex_lock(&lock);int slot=0;while(slot<ROM_DIR_MAX&&rom_dirs[slot].used)slot++;
 if(slot<ROM_DIR_MAX){rom_dirs[slot].used=1;rom_dirs[slot].entries=list;rom_dirs[slot].count=unique;rom_dirs[slot].pos=0;*out=ROM_DIR_BASE+(uint64_t)slot;}
 pthread_mutex_unlock(&lock);if(slot==ROM_DIR_MAX){free(list);return INVALID;}return 0;
}
unsigned runtime_romfs_directory_read(int64_t *count,RuntimeDirectoryEntry *out,uint64_t h,int64_t max){
 if(!runtime_romfs_directory_handle(h)||!count||max<0||(!out&&max))return INVALID;
 pthread_mutex_lock(&lock);size_t i=(size_t)(h-ROM_DIR_BASE);if(!rom_dirs[i].used){pthread_mutex_unlock(&lock);return INVALID;}
 size_t n=rom_dirs[i].count-rom_dirs[i].pos;if((uint64_t)max<n)n=(size_t)max;
 if(n)memcpy(out,rom_dirs[i].entries+rom_dirs[i].pos,n*sizeof *out);rom_dirs[i].pos+=n;*count=(int64_t)n;pthread_mutex_unlock(&lock);return 0;
}
unsigned runtime_romfs_directory_count(int64_t *out,uint64_t h){
 if(!out||!runtime_romfs_directory_handle(h))return INVALID;pthread_mutex_lock(&lock);size_t i=(size_t)(h-ROM_DIR_BASE);int used=rom_dirs[i].used;if(used)*out=(int64_t)rom_dirs[i].count;pthread_mutex_unlock(&lock);return used?0:INVALID;
}
void runtime_romfs_directory_close(uint64_t h){need(runtime_romfs_directory_handle(h),"directory handle range");pthread_mutex_lock(&lock);size_t i=(size_t)(h-ROM_DIR_BASE);need(rom_dirs[i].used,"close valid directory handle");free(rom_dirs[i].entries);memset(&rom_dirs[i],0,sizeof rom_dirs[i]);pthread_mutex_unlock(&lock);}
