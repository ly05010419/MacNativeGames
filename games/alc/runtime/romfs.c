/* Read-only game RomFS, backed by the player's fully IVFC-verified image. */
#include "runtime.h"
#include "totk_ui_lang.h"
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
/* Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
   path, the overlay's size and SHA-256 and the original's SHA-256; a missing path, a size or hash mismatch or a changed
   original stops the start. The directories, in this order: the build's patches (scripts/make_romfs_patches.py;
   TOTK_ROMFS_PATCHES, default extracted/<version>/romfs-patches, 0 = none), the true-2K HeapDef overlay
   (scripts/make_output_res_overlay.py) and the player's mods (TOTK_ROMFS_MODS, which the launcher sets to
   <data>/mods/<version>; no manifest there = none). Each file can be replaced once. An overlay file is opened when the
   game opens it, so a mod can replace thousands of files (a dub's voice streams) without holding a descriptor each; the
   file's identity (device, inode, size, modification time) is kept from the start check, and an open that finds another
   file there stops the game instead of serving bytes nobody checked. scripts/make_mod_manifest.py writes a mod's manifest.
   A line whose original SHA-256 is "-" adds a file the RomFS lacks (issue #48: The Second Quest adds 874): it is numbered
   after the RomFS's own files (file_count + k, k in path order, added[]), opens and reads like a replacement, and its
   folders exist for runtime_romfs_entry_type. The RomFS has no directory listing here, so nothing else needs to know. */
typedef struct { uint64_t index,size; char *path; dev_t dev; ino_t ino; struct timespec mtime; char *name; } Overlay;
static Overlay *overlays; /* sorted by RomFS index after overlay_init */
static size_t noverlay,overlay_cap;
#define ADDED UINT64_MAX   /* an added file's index until overlay_init numbers it */
static char **added;       /* RomFS paths of the added files, sorted; added[k] is index file_count + k */
static size_t nadded;
static uint32_t handle_ovl[NH]; /* overlay slot + 1 of an open handle, 0 = original */
static int handle_fd[NH];       /* the handle's open overlay file, when handle_ovl is set */
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
  if(ok&&!force&&!memcmp(saved,want,bytes)){fprintf(stderr,"[totk] RomFS parts unchanged; reused full SHA256 verification record (%u parts)\n",nchunks);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 over %u parts (%llu bytes)%s\n",nchunks,(unsigned long long)ROMFS_IMAGE_SIZE,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,ROMFS_IMAGE_SIZE);sha_fd(image_fd,ROMFS_IMAGE_SHA256,ROMFS_IMAGE_SIZE,TOTK_UI("校验游戏资源","Verifying game data"));
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
  if(ok&&!force&&proof_equal(&saved,&want)){fprintf(stderr,"[totk] RomFS unchanged; reused full SHA256 verification record (%llu bytes)\n",(unsigned long long)want.size);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),want.size,want.size);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 (%llu bytes)%s\n",(unsigned long long)want.size,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,want.size);sha_fd(fd,ROMFS_IMAGE_SHA256,want.size,TOTK_UI("校验游戏资源","Verifying game data"));
 need(fstat(fd,&after)==0,"RomFS image stat after hash");
 RomfsProof finished=proof_for(&after);need(proof_equal(&want,&finished),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,&want,sizeof(want));int ok=n==sizeof(want)&&fsync(out)==0;close(out);
  if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void sha_range(int fd,uint64_t offset,uint64_t size,const char *expected,const char *what) {
 size_t cap=size<(8u<<20)?(size_t)size:(8u<<20);CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(cap?cap:1);need(buffer!=NULL,"hash buffer");
 for(uint64_t done=0;done<size;){size_t chunk=size-done>cap?cap:(size_t)(size-done);ssize_t n=src_pread(fd,buffer,chunk,(off_t)(offset+done));need(n>0,"read hash input");CC_SHA256_Update(&c,buffer,(CC_LONG)n);done+=(uint64_t)n;}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);
 if(strcmp(hex,expected)){fprintf(stderr,"[totk] RomFS overlay: %s SHA-256 %s, manifest says %s\n",what,hex,expected);runtime_fail("RomFS overlay integrity mismatch");}
}
static uint64_t lower(const char *name);
static int path_ok(const char *p);
static void overlay_load(const char *dir,int required) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"RomFS overlay path");FILE *m=fopen(path,"r");
 if(!m&&required){fprintf(stderr,"[totk] RomFS overlay: %s missing; build it with scripts/make_output_res_overlay.py <w>x<h>\n",path);runtime_fail("true-2K HeapDef overlay missing (make romfs-patches-out OUTPUT_RES=<w>x<h>)");}
 if(!m){fprintf(stderr,"[totk] RomFS overlay: none (%s)\n",path);return;}
 char line[8192];size_t first=noverlay;uint64_t hashed=0,t0=runtime_now_ns();
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;char name[4096],sha[65],orig[65];unsigned long long size=0;
  need(sscanf(line,"%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]",name,&size,sha,orig)==4&&strlen(sha)==64&&(strlen(orig)==64||!strcmp(orig,"-")),"RomFS overlay manifest line");
  need(path_ok(name),"RomFS overlay path");uint64_t i=lower(name);int known=i<file_count&&!strcmp(entry_name(entry(i)),name),add=!strcmp(orig,"-");
  if(!known&&!add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay names a file the RomFS lacks");}
  if(known&&add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay adds a file the RomFS already has");}
  Entry e={0};if(known){e=entry(i);sha_range(image_fd,e.offset,e.size,orig,"original");}else i=ADDED;
  char file[8200];need(snprintf(file,sizeof(file),"%s/%s",dir,name)<(int)sizeof(file),"RomFS overlay file path");int fd=open(file,O_RDONLY|O_CLOEXEC);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"RomFS overlay file missing or wrong size");sha_range(fd,0,size,sha,name);close(fd);
  hashed+=e.size+size;
  if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
  char *copy=strdup(file),*rel=strdup(name);need(copy&&rel,"RomFS overlay path copy");overlays[noverlay++]=(Overlay){i,size,copy,st.st_dev,st.st_ino,st.st_mtimespec,rel};
  if(noverlay-first<=16)fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) %s\n",name,size,add?"is added":"replaces the original");
 }
 fclose(m);
 if(noverlay-first>16)fprintf(stderr,"[totk] RomFS overlay: %zu files from %s (%zu more not listed)\n",noverlay-first,dir,noverlay-first-16);
 if(noverlay>first)fprintf(stderr,"[totk] RomFS overlay: %s checked, %zu files, %.1f MB hashed in %.2f s\n",dir,noverlay-first,hashed/1048576.0,(runtime_now_ns()-t0)/1e9);
}
static int overlay_order(const void *a,const void *b) {uint64_t x=((const Overlay *)a)->index,y=((const Overlay *)b)->index;return (x>y)-(x<y);}
static int added_order(const void *a,const void *b) {const Overlay *x=*(Overlay *const *)a,*y=*(Overlay *const *)b;return strcmp(x->name,y->name);}
/* Number the added files in path order after the RomFS's own (file_count + k); two overlays adding one path stop the start. */
static void added_init(void) {
 Overlay **list=NULL;for(size_t k=0;k<noverlay;k++)if(overlays[k].index==ADDED){list=realloc(list,(nadded+1)*sizeof(*list));need(list!=NULL,"RomFS added files");list[nadded++]=&overlays[k];}
 if(!nadded)return;
 qsort(list,nadded,sizeof(*list),added_order);added=malloc(nadded*sizeof(*added));need(added!=NULL,"RomFS added files");
 for(size_t k=0;k<nadded;k++){
  if(k&&!strcmp(list[k]->name,list[k-1]->name)){fprintf(stderr,"[totk] RomFS overlay: %s and %s both add %s\n",list[k-1]->path,list[k]->path,list[k]->name);runtime_fail("RomFS overlay: two overlays add the same file (see the log; remove it from one of the manifests)");}
  list[k]->index=file_count+k;added[k]=list[k]->name;}
 free(list);fprintf(stderr,"[totk] RomFS overlay: %zu files added to the RomFS\n",nadded);
}
/* The index of RomFS path p (an added file: file_count + k), or UINT64_MAX. */
static uint64_t file_index(const char *p) {
 uint64_t i=lower(p);if(i<file_count&&!strcmp(p,entry_name(entry(i))))return i;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],p)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strcmp(added[lo],p)?file_count+lo:UINT64_MAX;
}
/* 1 when a RomFS file or an added one lies under folder (path ending in '/'). */
static int folder_exists(const char *folder) {
 size_t n=strlen(folder);uint64_t i=lower(folder);if(i<file_count&&!strncmp(entry_name(entry(i)),folder,n))return 1;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],folder)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strncmp(added[lo],folder,n);
}
/* The overlay slot + 1 that replaces RomFS file i, or 0. */
static uint32_t overlay_slot(uint64_t i) {size_t lo=0,hi=noverlay;while(lo<hi){size_t m=lo+(hi-lo)/2;if(overlays[m].index<i)lo=m+1;else hi=m;}return lo<noverlay&&overlays[lo].index==i?(uint32_t)(lo+1):0;}
/* TOTK_ROMFS_PATCHES (default extracted/<version>/romfs-patches; 0 = none); with a true-2K output bigger than 1920x1080
   also <TOTK_ROMFS_PATCHES_OUT, default extracted/<version>/romfs-patches-out>/<w>x<h> (the HeapDef sizes,
   scripts/make_output_res_overlay.py); then the player's mods in TOTK_ROMFS_MODS (0 = none). */
static void overlay_init(void) {
 const char *dir=getenv("TOTK_ROMFS_PATCHES");if(!dir||strcmp(dir,"0"))overlay_load(dir?dir:"extracted/" TOTK_GAME_DIR "/romfs-patches",0);
 unsigned w,h;if(runtime_output_res(&w,&h)&&runtime_output_res_extra_bytes()){const char *out=getenv("TOTK_ROMFS_PATCHES_OUT");char d[4096];
  need(snprintf(d,sizeof(d),"%s/%ux%u",out?out:"extracted/" TOTK_GAME_DIR "/romfs-patches-out",w,h)<(int)sizeof(d),"RomFS output overlay path");overlay_load(d,1);}
 const char *mods=getenv("TOTK_ROMFS_MODS");if(mods&&*mods&&strcmp(mods,"0"))overlay_load(mods,0);
 added_init();
 if(noverlay)qsort(overlays,noverlay,sizeof(*overlays),overlay_order);
 for(size_t k=1;k<noverlay;k++)if(overlays[k].index==overlays[k-1].index){
  fprintf(stderr,"[totk] RomFS overlay: %s and %s replace the same file %s\n",overlays[k-1].path,overlays[k].path,overlays[k].name);
  runtime_fail("RomFS overlay: two overlays replace the same file (see the log; remove it from one of the manifests)");}
}
void runtime_fs_init(void) {
 if(image_fd>=0)return;
 const char *dir=getenv("TOTK_ROMFS");if(!dir)dir="extracted/" TOTK_GAME_DIR "/romfs";char path[4096];
 need(snprintf(path,sizeof(path),"%s/runtime-index.bin",dir)<(int)sizeof(path),"RomFS path");int fd=open(path,O_RDONLY);need(fd>=0,"RomFS index missing");
 struct stat st;need(fstat(fd,&st)==0&&st.st_size>=24&&st.st_size<128*1024*1024,"RomFS index size");runtime_boot_progress(TOTK_UI("读取资源索引","Reading the data index"),0,0);sha_fd(fd,ROMFS_INDEX_SHA256,0,NULL);
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
 runtime_boot_status(TOTK_UI("准备游戏资源","Preparing game data"),TOTK_UI("检查分辨率补丁并挂载只读游戏资源","Checking the resolution patch and mounting the read-only game data"));
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
static unsigned mount_rom_name(const char *name) {return mount_rom(name,NULL,0);}   /* SDK 13 MountRom without a cache buffer (Astral Chain) */
static unsigned cache_size(uint64_t *out) {if(!out)return INVALID;*out=0;return 0;}
/* TOTK_FS_LOG=1: log every RomFS open with the time since start (shows how far the game gets without a picture). */
static void fs_log(const char *what,const char *path){static int on=-1;static uint64_t t0;if(on<0){on=getenv("TOTK_FS_LOG")!=NULL;t0=runtime_now_ns();}if(on)fprintf(stderr,"[totk-fs] %8.3f %s %s\n",(runtime_now_ns()-t0)/1e9,what,path);}
void runtime_fs_log(const char *what,const char *path){fs_log(what,path);}
unsigned runtime_romfs_open(uint64_t *out,const char *path,int mode) {
 fs_log("rom",path?path:"(null)");
 if(!out||mode!=1)return UNSUPPORTED;const char *p=rom_path(path);if(!p)return NOT_FOUND;uint64_t i=file_index(p);if(i==UINT64_MAX)return NOT_FOUND;
 uint32_t ov=overlay_slot(i);int fd=-1;
 if(ov){const Overlay *o=&overlays[ov-1];fd=open(o->path,O_RDONLY|O_CLOEXEC);struct stat st;
  if(fd<0||fstat(fd,&st)!=0||(uint64_t)st.st_size!=o->size||st.st_dev!=o->dev||st.st_ino!=o->ino||st.st_mtimespec.tv_sec!=o->mtime.tv_sec||st.st_mtimespec.tv_nsec!=o->mtime.tv_nsec){
   fprintf(stderr,"[totk] RomFS overlay: %s is missing or changed since the start check\n",o->path);runtime_fail("RomFS overlay file changed while the game ran (restart the game)");}}
 pthread_mutex_lock(&lock);for(unsigned j=1;j<NH;j++)if(!handles[j]){handles[j]=i+1;handle_ovl[j]=ov;handle_fd[j]=fd;*out=0x100000000ULL+j;pthread_mutex_unlock(&lock);return 0;}pthread_mutex_unlock(&lock);runtime_fail("file handle table full");return INVALID;
}
static int handle_entry(uint64_t h,Entry *e) {if(h<=0x100000000ULL||h>=0x100000000ULL+NH)return 0;pthread_mutex_lock(&lock);uint64_t i=handles[h-0x100000000ULL];pthread_mutex_unlock(&lock);if(!i)return 0;*e=i-1<file_count?entry(i-1):(Entry){0};return 1;}   /* an added file's bytes are all in its overlay */
/* Where an open handle's bytes come from: the overlay file, or the original's range of the image. */
static void handle_source(uint64_t h,const Entry *e,int *fd,uint64_t *base,uint64_t *size) {
 pthread_mutex_lock(&lock);uint32_t ov=handle_ovl[h-0x100000000ULL];int ofd=handle_fd[h-0x100000000ULL];pthread_mutex_unlock(&lock);
 if(ov){*fd=ofd;*base=0;*size=overlays[ov-1].size;}else{*fd=image_fd;*base=e->offset;*size=e->size;}
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
void runtime_romfs_close(uint64_t h) {Entry e;if(!handle_entry(h,&e))runtime_fail("close invalid file handle");uint64_t j=h-0x100000000ULL;pthread_mutex_lock(&lock);int fd=handle_ovl[j]?handle_fd[j]:-1;handles[j]=0;handle_ovl[j]=0;handle_fd[j]=-1;pthread_mutex_unlock(&lock);if(fd>=0)close(fd);}
unsigned runtime_romfs_entry_type(int *out,const char *path) {
 if(!out)return INVALID;const char *p=rom_path(path);if(!p)return NOT_FOUND;if(file_index(p)!=UINT64_MAX){*out=1;return 0;}
 char dir[4096];if(snprintf(dir,sizeof(dir),"%s/",p)>=(int)sizeof(dir))return INVALID;if(folder_exists(dir)){*out=0;return 0;}return NOT_FOUND;
}
/* Directory listing (Astral Chain lists Contents:/): the immediate children of a folder, from the sorted index and the
   added files. A name with no further '/' is a file; the first segment of a longer one is a subfolder, and the sorted
   order keeps a subfolder's paths together, so each is reported once per source. */
static void list_source(const char *prefix,size_t n,int mode,const char *(*name_at)(uint64_t),uint64_t (*size_at)(uint64_t),uint64_t from,uint64_t to,
                        void (*add)(void *,const char *,int,int64_t),void *ctx,char *last_dir) {
 for(uint64_t i=from;i<to;i++){const char *full=name_at(i);if(strncmp(full,prefix,n))break;const char *rest=full+n,*slash=strchr(rest,'/');
  if(!slash){if(mode&2)add(ctx,rest,0,(int64_t)size_at(i));continue;}
  size_t k=(size_t)(slash-rest);if(k>=768)continue;if(!strncmp(last_dir,rest,k)&&!last_dir[k])continue;
  memcpy(last_dir,rest,k);last_dir[k]=0;if(mode&1)add(ctx,last_dir,1,0);}
}
static const char *list_index_name(uint64_t i){return entry_name(entry(i));}
static uint64_t list_index_size(uint64_t i){return entry(i).size;}
static const char *added_name(uint64_t k){return added[k];}
static uint64_t added_size(uint64_t k){char full[4200];snprintf(full,sizeof full,"%s:/%s",mount_name,added[k]);uint64_t h;int64_t size=0;if(runtime_romfs_open(&h,full,1))return 0;runtime_romfs_size(&size,h);runtime_romfs_close(h);return (uint64_t)size;}
unsigned runtime_romfs_list(const char *path,int mode,void (*add)(void *ctx,const char *name,int is_dir,int64_t size),void *ctx) {
 /* A folder may be named with trailing slashes, and the mount root ("Contents:/") is the empty path; files keep the strict check. */
 char trimmed[4096];if(!path||snprintf(trimmed,sizeof trimmed,"%s",path)>=(int)sizeof trimmed)return INVALID;
 size_t tl=strlen(trimmed),ml=strlen(mount_name);while(tl>ml+2&&trimmed[tl-1]=='/')trimmed[--tl]=0;
 int root=!strncmp(trimmed,mount_name,ml)&&trimmed[ml]==':'&&(!trimmed[ml+1]||(trimmed[ml+1]=='/'&&!trimmed[ml+2]));
 const char *p=root?"":rom_path(trimmed);if(!p)return NOT_FOUND;char prefix[4096];
 if(snprintf(prefix,sizeof prefix,"%s%s",p,*p&&p[strlen(p)-1]!='/'?"/":"")>=(int)sizeof prefix)return INVALID;
 size_t n=strlen(prefix);if(n&&!folder_exists(prefix))return NOT_FOUND;
 char last[768]="";list_source(prefix,n,mode,list_index_name,list_index_size,n?lower(prefix):0,file_count,add,ctx,last);
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],prefix)<0)lo=m+1;else hi=m;}
 last[0]=0;list_source(prefix,n,mode,added_name,added_size,lo,nadded,add,ctx,last);
 return 0;
}
int runtime_romfs_owns(const char *path) {size_t n=strlen(mount_name);return n&&path&&!strncmp(path,mount_name,n)&&path[n]==':';}
int runtime_romfs_handle(uint64_t h) {return h>0x100000000ULL&&h<0x100000000ULL+NH;}
unsigned runtime_romfs_read_options(uint64_t *out,uint64_t h,int64_t off,void *buf,uint64_t size) {return runtime_romfs_read(out,h,off,buf,size);}
void *runtime_fs_override(const char *name) {
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn2fs8MountRomEPKcPvm",mount_rom},{"_ZN2nn2fs8MountRomEPKc",mount_rom_name},{"_ZN2nn2fs22QueryMountRomCacheSizeEPm",cache_size}};
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
 for(size_t k=0;k<noverlay;k++){char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",overlays[k].name)<(int)sizeof(p),"probe overlay path");
  if(overlays[k].index>=file_count){int type=-1;char *slash=strchr(overlays[k].name,'/')?strrchr(p,'/'):NULL;
   if(slash){*slash=0;need(!runtime_romfs_entry_type(&type,p)&&type==0,"probe added file's folder");*slash='/';}
   need(!runtime_romfs_entry_type(&type,p)&&type==1,"probe added file type");}uint64_t oh=0,n=0;int64_t size=0;uint8_t a[64],b[64];
  need(!runtime_romfs_open(&oh,p,1)&&!runtime_romfs_size(&size,oh)&&(uint64_t)size==overlays[k].size,"probe overlay size");uint64_t tail=overlays[k].size<64?overlays[k].size:64;int fd=open(overlays[k].path,O_RDONLY|O_CLOEXEC);
  need(fd>=0&&!runtime_romfs_read(&n,oh,size-(int64_t)tail,a,tail)&&n==tail&&pread(fd,b,tail,(off_t)(size-(int64_t)tail))==(ssize_t)tail&&!memcmp(a,b,tail),"probe overlay bytes");close(fd);runtime_romfs_close(oh);}
 uint64_t h=0;need(runtime_romfs_open(&h,"rom:/../main",1)==NOT_FOUND,"reject traversal");need(runtime_romfs_open(&h,"rom:/absent",2)==UNSUPPORTED,"reject write open");
 printf("{\"status\":\"pass\",\"files\":%llu,\"sampled_files\":%u,\"overlays\":%zu,\"added\":%zu,\"read_digest\":\"%016llx\",\"scope\":\"read-only mount/open/size/read/EOF, verified overlays and added files; no game startup\"}\n",(unsigned long long)file_count,count,noverlay,nadded,(unsigned long long)sum);
}
