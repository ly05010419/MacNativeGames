/* Read-only game RomFS, backed by the player's fully IVFC-verified image. */
#include "runtime.h"
#include "romfs_config.h"
#include "game_version.h"
#include "fs_host.h"
#include "romfs_path.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536
static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count,strings_offset;
static int image_fd=-1;
/* Split image: romfs.bin.000, .001, ... of equal size (the last one shorter), read in place. The phone gets the 17 GB
   image this way: devicectl drops the connection on a single file of that size, and joining the parts on the device
   would need the space twice. image_fd is then the first part; src_pread routes image offsets to their part. */
#define MAX_CHUNKS 64
static int chunk_fds[MAX_CHUNKS];
static unsigned nchunks;
static uint64_t chunk_size;
static ssize_t src_pread(int fd,void *buf,size_t n,off_t off) {
 if(!nchunks||fd!=image_fd)return pread(fd,buf,n,off);
 uint64_t c=(uint64_t)off/chunk_size,o=(uint64_t)off%chunk_size;if(c>=nchunks)return 0;
 if(n>chunk_size-o)n=(size_t)(chunk_size-o);
 return pread(chunk_fds[c],buf,n,(off_t)o);
}
static const uint8_t *image_map;
static char mount_name[32];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t read_calls,read_bytes,read_ns,read_prepare_ns,read_report_ns;
static uint64_t handles[NH]; /* file index + 1; 0 is free */
/* Overlay (scripts/make_romfs_patches.py): files listed in <TOTK_ROMFS_PATCHES, default extracted/v0/romfs-patches>/manifest.tsv
   are served in place of the originals. Each line names a RomFS path, the overlay's size and SHA-256 and the original's
   SHA-256; a missing path, a size or hash mismatch or a changed original stops the start (regenerate the overlay).
   TOTK_ROMFS_PATCHES=0 serves the originals. */
#define NOVL 255   /* handle_ovl is uint8_t: slot+1 must fit; 255 leaves ~252 files for user mods */
static struct { uint64_t index,size; int fd; } overlays[NOVL];
static unsigned noverlay;
static uint8_t handle_ovl[NH]; /* overlay slot + 1 of an open handle, 0 = original */
typedef struct { uint64_t offset,size;uint32_t name,len; } Entry;
_Static_assert(sizeof(Entry)==24,"index entry layout");
static Entry entry(uint64_t i) {Entry e;memcpy(&e,index_bytes+24+i*24,24);return e;}
static const char *entry_name(Entry e) {return (char *)index_bytes+strings_offset+e.name;}
static void need(int c,const char *m) {if(!c)runtime_fail(m);}
static void sha_fd(int fd,const char *expected,uint64_t total,const char *stage) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(off_t off=0;;){ssize_t n=src_pread(fd,buffer,8<<20,off);need(n>=0,"read hash input");if(!n)break;CC_SHA256_Update(&c,buffer,(CC_LONG)n);off+=n;
  if(stage&&((uint64_t)off%(256u<<20)<(8u<<20)||!n))runtime_boot_progress(stage,(uint64_t)off,total);}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);need(!strcmp(hex,expected),"RomFS integrity mismatch");
}
/* A successful full hash may be reused only while the same regular file has
   unchanged filesystem identity and modification metadata. This is a startup
   shortcut, not a fresh integrity proof; TOTK_ROMFS_VERIFY=full forces one. */
typedef struct {
 char magic[16], sha256[65];
 uint64_t dev,ino,size;
 int64_t mtime_sec,mtime_nsec,ctime_sec,ctime_nsec,birth_sec,birth_nsec;
} RomfsProof;
static RomfsProof proof_for(const struct stat *s) {
 RomfsProof p={0};memcpy(p.magic,"TOTKRFS-PROOF-1",16);memcpy(p.sha256,ROMFS_IMAGE_SHA256,65);
 p.dev=(uint64_t)s->st_dev;p.ino=(uint64_t)s->st_ino;p.size=(uint64_t)s->st_size;
 p.mtime_sec=s->st_mtimespec.tv_sec;p.mtime_nsec=s->st_mtimespec.tv_nsec;
 p.ctime_sec=s->st_ctimespec.tv_sec;p.ctime_nsec=s->st_ctimespec.tv_nsec;
 p.birth_sec=s->st_birthtimespec.tv_sec;p.birth_nsec=s->st_birthtimespec.tv_nsec;
 return p;
}
static int proof_equal(const RomfsProof *a,const RomfsProof *b) {
 return !memcmp(a->magic,b->magic,sizeof a->magic)&&!memcmp(a->sha256,b->sha256,sizeof a->sha256)
  &&a->dev==b->dev&&a->ino==b->ino&&a->size==b->size
  &&a->mtime_sec==b->mtime_sec&&a->mtime_nsec==b->mtime_nsec
  &&a->ctime_sec==b->ctime_sec&&a->ctime_nsec==b->ctime_nsec
  &&a->birth_sec==b->birth_sec&&a->birth_nsec==b->birth_nsec;
}
/* Split image: one record per part, all of which must be unchanged for the earlier full hash to be reused. */
static void chunk_proofs(RomfsProof *out) {
 /* ctime is left out: reinstalling the iOS app touches the data container's file metadata (ctime) without changing a
    byte, which made every launch hash 17 GB again. A content change still moves mtime (and size). */
 for(unsigned c=0;c<nchunks;c++){struct stat st;need(fstat(chunk_fds[c],&st)==0&&S_ISREG(st.st_mode),"RomFS part stat");out[c]=proof_for(&st);out[c].ctime_sec=out[c].ctime_nsec=0;}
}
static void verify_chunks(const char *dir) {
 RomfsProof want[MAX_CHUNKS],saved[MAX_CHUNKS],after[MAX_CHUNKS];size_t bytes=nchunks*sizeof(RomfsProof);chunk_proofs(want);
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;   /* iOS: directories devicectl creates belong to root */
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-parts-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&(size_t)ps.st_size==bytes&&read(proof_fd,saved,bytes)==(ssize_t)bytes;close(proof_fd);
  if(ok&&!force&&!memcmp(saved,want,bytes)){fprintf(stderr,"[totk] RomFS parts unchanged; reused full SHA256 verification record (%u parts)\n",nchunks);runtime_boot_progress("游戏资源校验记录有效",ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 over %u parts (%llu bytes)%s\n",nchunks,(unsigned long long)ROMFS_IMAGE_SIZE,force?" [forced]":"");
 runtime_boot_progress("校验游戏资源",0,ROMFS_IMAGE_SIZE);sha_fd(image_fd,ROMFS_IMAGE_SHA256,ROMFS_IMAGE_SIZE,"校验游戏资源");
 chunk_proofs(after);need(!memcmp(after,want,bytes),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-parts-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,want,bytes);int ok=n==(ssize_t)bytes&&fsync(out)==0;close(out);if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void open_chunks(const char *dir) {
 uint64_t total=0;
 for(;nchunks<MAX_CHUNKS;nchunks++){char path[4096];need(snprintf(path,sizeof(path),"%s/romfs.bin.%03u",dir,nchunks)<(int)sizeof(path),"RomFS part path");
  int fd=open(path,O_RDONLY);if(fd<0)break;struct stat st;need(fstat(fd,&st)==0&&st.st_size>0,"RomFS part stat");
  if(!nchunks)chunk_size=(uint64_t)st.st_size;else need(chunk_fds[nchunks-1]>=0&&total==(uint64_t)nchunks*chunk_size,"RomFS parts must have equal sizes except the last");
  chunk_fds[nchunks]=fd;total+=(uint64_t)st.st_size;}
 need(nchunks>0,"RomFS image missing (romfs.bin or romfs.bin.000...)");
 need(total==ROMFS_IMAGE_SIZE,"RomFS parts do not add up to the image size");
 image_fd=chunk_fds[0];
 fprintf(stderr,"[totk] RomFS image in %u parts of %llu bytes\n",nchunks,(unsigned long long)chunk_size);
}
static void verify_image(int fd,const char *dir) {
 if(nchunks){verify_chunks(dir);return;}
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;
 struct stat before,after;need(fstat(fd,&before)==0&&S_ISREG(before.st_mode),"RomFS image stat");
 RomfsProof want=proof_for(&before),saved;
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&ps.st_size==sizeof(saved)&&read(proof_fd,&saved,sizeof(saved))==sizeof(saved);close(proof_fd);
  if(ok&&!force&&proof_equal(&saved,&want)){fprintf(stderr,"[totk] RomFS unchanged; reused full SHA256 verification record (%llu bytes)\n",(unsigned long long)want.size);runtime_boot_progress("游戏资源校验记录有效",want.size,want.size);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 (%llu bytes)%s\n",(unsigned long long)want.size,force?" [forced]":"");
 runtime_boot_progress("校验游戏资源",0,want.size);sha_fd(fd,ROMFS_IMAGE_SHA256,want.size,"校验游戏资源");
 need(fstat(fd,&after)==0,"RomFS image stat after hash");
 RomfsProof finished=proof_for(&after);need(proof_equal(&want,&finished),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,&want,sizeof(want));int ok=n==sizeof(want)&&fsync(out)==0;close(out);
  if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void sha_range(int fd,uint64_t offset,uint64_t size,const char *expected,const char *what) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(uint64_t done=0;done<size;){size_t chunk=size-done>(8u<<20)?(8u<<20):(size_t)(size-done);ssize_t n=src_pread(fd,buffer,chunk,(off_t)(offset+done));need(n>0,"read hash input");CC_SHA256_Update(&c,buffer,(CC_LONG)n);done+=(uint64_t)n;}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);
 if(strcmp(hex,expected)){fprintf(stderr,"[totk] RomFS overlay: %s SHA-256 %s, manifest says %s\n",what,hex,expected);runtime_fail("RomFS overlay integrity mismatch");}
}
static uint64_t lower(const char *name);
static int path_ok(const char *p);
static void overlay_load(const char *dir,int required) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"RomFS overlay path");FILE *m=fopen(path,"r");
 if(!m&&required){fprintf(stderr,"[totk] RomFS overlay: %s missing; build it with scripts/make_output_res_overlay.py <w>x<h>\n",path);runtime_fail("true-2K HeapDef overlay missing (make romfs-patches-out OUTPUT_RES=<w>x<h>)");}
 if(!m){fprintf(stderr,"[totk] RomFS overlay: none (%s)\n",path);return;}
 char line[8192];
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;char name[4096],sha[65],orig[65];unsigned long long size=0;
  need(sscanf(line,"%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f]",name,&size,sha,orig)==4&&strlen(sha)==64&&strlen(orig)==64,"RomFS overlay manifest line");
  need(path_ok(name)&&noverlay<NOVL,"RomFS overlay path or count");uint64_t i=lower(name);need(i<file_count&&!strcmp(entry_name(entry(i)),name),"RomFS overlay names a file the RomFS lacks");
  for(unsigned k=0;k<noverlay;k++)need(overlays[k].index!=i,"RomFS overlay: two overlays replace the same file");
  Entry e=entry(i);sha_range(image_fd,e.offset,e.size,orig,"original");
  char file[8200];need(snprintf(file,sizeof(file),"%s/%s",dir,name)<(int)sizeof(file),"RomFS overlay file path");int fd=open(file,O_RDONLY);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"RomFS overlay file missing or wrong size");sha_range(fd,0,size,sha,name);
  overlays[noverlay].index=i;overlays[noverlay].size=size;overlays[noverlay].fd=fd;noverlay++;
  fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) replaces the original\n",name,size);
 }
 fclose(m);
}
/* TOTK_ROMFS_PATCHES (default extracted/v0/romfs-patches; 0 = none), and with a true-2K output bigger than 1920x1080 also
   <TOTK_ROMFS_PATCHES_OUT, default extracted/v0/romfs-patches-out>/<w>x<h> (the HeapDef sizes, scripts/make_output_res_overlay.py). */
static void overlay_init(void) {
 const char *dir=getenv("TOTK_ROMFS_PATCHES");if(!dir||strcmp(dir,"0"))overlay_load(dir?dir:"extracted/" TOTK_GAME_DIR "/romfs-patches",0);
 unsigned w,h;if(runtime_output_res(&w,&h)&&runtime_output_res_extra_bytes()){const char *out=getenv("TOTK_ROMFS_PATCHES_OUT");char d[4096];
  need(snprintf(d,sizeof(d),"%s/%ux%u",out?out:"extracted/" TOTK_GAME_DIR "/romfs-patches-out",w,h)<(int)sizeof(d),"RomFS output overlay path");overlay_load(d,1);}
}
void runtime_fs_init(void) {
 if(image_fd>=0)return;
 const char *dir=getenv("TOTK_ROMFS");if(!dir)dir="extracted/" TOTK_GAME_DIR "/romfs";char path[4096];
 need(snprintf(path,sizeof(path),"%s/runtime-index.bin",dir)<(int)sizeof(path),"RomFS path");int fd=open(path,O_RDONLY);need(fd>=0,"RomFS index missing");
 struct stat st;need(fstat(fd,&st)==0&&st.st_size>=24&&st.st_size<128*1024*1024,"RomFS index size");runtime_boot_progress("读取资源索引",0,0);sha_fd(fd,ROMFS_INDEX_SHA256,0,NULL);
 index_size=(size_t)st.st_size;index_bytes=mmap(NULL,index_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);need(index_bytes!=MAP_FAILED,"map RomFS index");
 need(!memcmp(index_bytes,"TOTKRFS1",8),"RomFS index format");memcpy(&file_count,index_bytes+8,8);memcpy(&strings_offset,index_bytes+16,8);
 need(file_count<1000000&&strings_offset==24+file_count*24&&strings_offset<index_size,"RomFS index layout");
 const char *previous=NULL;
 for(uint64_t i=0;i<file_count;i++){Entry e=entry(i);need(e.offset<=ROMFS_IMAGE_SIZE&&e.size<=ROMFS_IMAGE_SIZE-e.offset,"RomFS file bounds");need(e.name<index_size-strings_offset&&e.len>1&&e.len<=index_size-strings_offset-e.name,"RomFS name bounds");const char *name=entry_name(e);need(name[e.len-1]==0&&strlen(name)==e.len-1&&(!previous||strcmp(previous,name)<0),"RomFS sorted paths");previous=name;}
 need(snprintf(path,sizeof(path),"%s/romfs.bin",dir)<(int)sizeof(path),"RomFS image path");image_fd=open(path,O_RDONLY);
 if(image_fd<0)open_chunks(dir);
 else need(fstat(image_fd,&st)==0&&(uint64_t)st.st_size==ROMFS_IMAGE_SIZE,"RomFS image size");
 verify_image(image_fd,dir);
 if(!nchunks&&getenv("TOTK_ROMFS_MMAP")&&strcmp(getenv("TOTK_ROMFS_MMAP"),"0")){
  void *mapped=mmap(NULL,(size_t)ROMFS_IMAGE_SIZE,PROT_READ,MAP_PRIVATE,image_fd,0);
  if(mapped!=MAP_FAILED){image_map=mapped;fprintf(stderr,"[totk] RomFS mmap read path enabled\n");}
  else fprintf(stderr,"[totk] RomFS mmap failed (%s); using pread\n",strerror(errno));
 }
 runtime_boot_status("准备游戏资源","检查分辨率补丁并挂载只读游戏资源");
 overlay_init();
 fprintf(stderr,"[totk] native read-only RomFS ready: %llu files\n",(unsigned long long)file_count);
}
static int path_ok(const char *p) {return romfs_path_valid_relative(p);}
static const char *rom_path(const char *path) {
 return romfs_path_relative(mount_name,path);
}
static uint64_t lower(const char *name) {uint64_t lo=0,hi=file_count;while(lo<hi){uint64_t m=lo+(hi-lo)/2;if(strcmp(entry_name(entry(m)),name)<0)lo=m+1;else hi=m;}return lo;}
static unsigned mount_rom(const char *name,void *cache,uint64_t size) {
 (void)cache;(void)size;runtime_fs_init();if(!name||!path_ok(name)||strlen(name)>=sizeof(mount_name)||strchr(name,'/')||strchr(name,':'))return INVALID;
 if(*mount_name&&strcmp(name,mount_name))runtime_fail("second RomFS mount not implemented");strcpy(mount_name,name);return 0;
}
static unsigned cache_size(uint64_t *out) {if(!out)return INVALID;*out=0;return 0;}
/* TOTK_FS_LOG=1: log every RomFS open with the time since start (shows how far the game gets without a picture). */
static void fs_log(const char *what,const char *path){static int on=-1;static uint64_t t0;if(on<0){on=getenv("TOTK_FS_LOG")!=NULL;t0=runtime_now_ns();}if(on)fprintf(stderr,"[totk-fs] %8.3f %s %s\n",(runtime_now_ns()-t0)/1e9,what,path);}
void runtime_fs_log(const char *what,const char *path){fs_log(what,path);}
unsigned runtime_romfs_open(uint64_t *out,const char *path,int mode) {
 fs_log("rom",path?path:"(null)");
 if(!out||mode!=1)return UNSUPPORTED;const char *p=rom_path(path);if(!p)return NOT_FOUND;uint64_t i=lower(p);if(i>=file_count||strcmp(p,entry_name(entry(i))))return NOT_FOUND;
 uint8_t ov=0;for(unsigned k=0;k<noverlay;k++)if(overlays[k].index==i)ov=(uint8_t)(k+1);
 pthread_mutex_lock(&lock);for(unsigned j=1;j<NH;j++)if(!handles[j]){handles[j]=i+1;handle_ovl[j]=ov;*out=0x100000000ULL+j;pthread_mutex_unlock(&lock);return 0;}pthread_mutex_unlock(&lock);runtime_fail("file handle table full");return INVALID;
}
static int handle_entry(uint64_t h,Entry *e) {if(h<=0x100000000ULL||h>=0x100000000ULL+NH)return 0;pthread_mutex_lock(&lock);uint64_t i=handles[h-0x100000000ULL];pthread_mutex_unlock(&lock);if(!i)return 0;*e=entry(i-1);return 1;}
/* Where an open handle's bytes come from: the overlay file, or the original's range of the image. */
static void handle_source(uint64_t h,const Entry *e,int *fd,uint64_t *base,uint64_t *size) {
 pthread_mutex_lock(&lock);uint8_t ov=handle_ovl[h-0x100000000ULL];pthread_mutex_unlock(&lock);
 if(ov){*fd=overlays[ov-1].fd;*base=0;*size=overlays[ov-1].size;}else{*fd=image_fd;*base=e->offset;*size=e->size;}
}
unsigned runtime_romfs_read(uint64_t *out,uint64_t h,int64_t offset,void *buffer,uint64_t size) {
 Entry e;if(!out||offset<0||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,fsize;handle_source(h,&e,&fd,&base,&fsize);if((uint64_t)offset>fsize||(!buffer&&size))return INVALID;
 uint64_t n=size;if(n>fsize-(uint64_t)offset)n=fsize-(uint64_t)offset;uint64_t done=0;
 static _Atomic int profile_mode=-1;int profile=atomic_load(&profile_mode);if(profile<0){profile=getenv("TOTK_FS_TIMING")!=NULL;atomic_store(&profile_mode,profile);}uint64_t prep_start=profile?runtime_now_ns():0;
 dm_guest_write_prepare(buffer,(size_t)n);
 uint64_t t0=profile?runtime_now_ns():0;
 if(fd==image_fd&&image_map){memcpy(buffer,image_map+base+(uint64_t)offset,(size_t)n);done=n;}
 else while(done<n){size_t chunk=(size_t)(n-done);if(chunk>8<<20)chunk=8<<20;ssize_t r=src_pread(fd,(uint8_t *)buffer+done,chunk,(off_t)(base+(uint64_t)offset+done));if(r<=0)runtime_fail("RomFS read failure");done+=(uint64_t)r;}
 if(profile){uint64_t now=runtime_now_ns();atomic_fetch_add(&read_calls,1);atomic_fetch_add(&read_bytes,done);atomic_fetch_add(&read_ns,now-t0);atomic_fetch_add(&read_prepare_ns,t0-prep_start);
  uint64_t last=atomic_load(&read_report_ns);
  if(now-last>1000000000ULL&&atomic_compare_exchange_strong(&read_report_ns,&last,now))
   fprintf(stderr,"[totk-fs] read window t=%.3f calls=%llu bytes=%llu wall_ms=%.3f prepare_ms=%.3f\n",now/1e9,
    (unsigned long long)atomic_exchange(&read_calls,0),(unsigned long long)atomic_exchange(&read_bytes,0),atomic_exchange(&read_ns,0)/1e6,atomic_exchange(&read_prepare_ns,0)/1e6);}
 *out=done;return 0;
}
unsigned runtime_romfs_size(int64_t *out,uint64_t h) {Entry e;if(!out||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,size;handle_source(h,&e,&fd,&base,&size);*out=(int64_t)size;return 0;}
void runtime_romfs_close(uint64_t h) {Entry e;if(!handle_entry(h,&e))runtime_fail("close invalid file handle");pthread_mutex_lock(&lock);handles[h-0x100000000ULL]=0;handle_ovl[h-0x100000000ULL]=0;pthread_mutex_unlock(&lock);}
unsigned runtime_romfs_entry_type(int *out,const char *path) {
 if(!out)return INVALID;const char *p=rom_path(path);if(!p)return NOT_FOUND;uint64_t i=lower(p);if(i<file_count&&!strcmp(entry_name(entry(i)),p)){*out=1;return 0;}
 char dir[4096];if(snprintf(dir,sizeof(dir),"%s/",p)>=(int)sizeof(dir))return INVALID;i=lower(dir);if(i<file_count&&!strncmp(entry_name(entry(i)),dir,strlen(dir))){*out=0;return 0;}return NOT_FOUND;
}
int runtime_romfs_owns(const char *path) {size_t n=strlen(mount_name);return n&&path&&!strncmp(path,mount_name,n)&&path[n]==':';}
int runtime_romfs_handle(uint64_t h) {return h>0x100000000ULL&&h<0x100000000ULL+NH;}
unsigned runtime_romfs_read_options(uint64_t *out,uint64_t h,int64_t off,void *buf,uint64_t size) {return runtime_romfs_read(out,h,off,buf,size);}
void *runtime_fs_override(const char *name) {
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn2fs8MountRomEPKcPvm",mount_rom},{"_ZN2nn2fs22QueryMountRomCacheSizeEPm",cache_size}};
 for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)if(!strcmp(name,map[i].name))return map[i].fn;return runtime_fs_host_override(name);
}
void runtime_fs_probe(void) {
 runtime_fs_init();need(mount_rom("rom",NULL,0)==0,"mount RomFS");unsigned count=0;uint8_t buffer[257];uint64_t sum=0;
 /* Audio stream paths include an extra slash after the mount root. Verify
    actual resource lookup and bytes, while still rejecting path traversal. */
 {
  const char *name="Sound/Resource/Stream/Dm_OP_0001.bwav";
  char path[256];uint8_t expected[257],actual[257];uint64_t h,n;
  snprintf(path,sizeof path,"rom:/%s",name);
  need(!runtime_romfs_open(&h,path,1),"open normal audio path");
  need(!runtime_romfs_read(&n,h,0,expected,sizeof expected)&&n==sizeof expected,"read normal audio path");runtime_romfs_close(h);
  for(int slashes=2;slashes<=3;slashes++){
   snprintf(path,sizeof path,slashes==2?"rom://%s":"rom:///%s",name);
   int type=-1;need(!runtime_romfs_entry_type(&type,path)&&type==1,"normalized audio entry type");
   need(!runtime_romfs_open(&h,path,1),"open normalized audio stream path");
   need(!runtime_romfs_read(&n,h,0,actual,sizeof actual)&&n==sizeof actual&&!memcmp(actual,expected,sizeof actual),"normalized audio stream bytes");runtime_romfs_close(h);
  }
  need(runtime_romfs_open(&h,"rom://../Sound/Resource/Stream/Dm_OP_0001.bwav",1)!=0,"reject traversal after repeated root separator");
 }
 for(uint64_t i=0;i<file_count;i+=file_count/64){Entry e=entry(i);char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe path");uint64_t h=0,n=0;int64_t size;need(!runtime_romfs_open(&h,p,1)&&!runtime_romfs_size(&size,h)&&(uint64_t)size==e.size,"probe open/size");need(!runtime_romfs_read(&n,h,0,buffer,sizeof(buffer)),"probe partial read");for(uint64_t j=0;j<n;j++)sum=sum*131+buffer[j];need(!runtime_romfs_read(&n,h,size,buffer,1)&&n==0,"probe EOF");runtime_romfs_close(h);count++;}
 for(unsigned k=0;k<noverlay;k++){Entry e=entry(overlays[k].index);char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe overlay path");uint64_t oh=0,n=0;int64_t size=0;uint8_t a[64],b[64];
  need(!runtime_romfs_open(&oh,p,1)&&!runtime_romfs_size(&size,oh)&&(uint64_t)size==overlays[k].size,"probe overlay size");need(!runtime_romfs_read(&n,oh,size-64,a,64)&&n==64&&pread(overlays[k].fd,b,64,(off_t)(size-64))==64&&!memcmp(a,b,64),"probe overlay bytes");runtime_romfs_close(oh);}
 uint64_t h=0;need(runtime_romfs_open(&h,"rom:/../main",1)==NOT_FOUND,"reject traversal");need(runtime_romfs_open(&h,"rom:/absent",2)==UNSUPPORTED,"reject write open");
 printf("{\"status\":\"pass\",\"files\":%llu,\"sampled_files\":%u,\"overlays\":%u,\"read_digest\":\"%016llx\",\"scope\":\"read-only mount/open/size/read/EOF, verified overlays; no game startup\"}\n",(unsigned long long)file_count,count,noverlay,(unsigned long long)sum);
}
