/*
 * PS Linker - resumable HTTP download (PS3 / PSL1GHT)
 *
 * A non-empty destination is resumed with Range: bytes=<size>-. 206 appends;
 * 200 (Range ignored) restarts.  The response's SHA-256 header is checked
 * when supplied; the computed digest is always logged for an auditable result.
 */
#include "resume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/net.h>

#define DL_BUF_SIZE (64 * 1024)
#ifdef NASDL_NETDEBUG
#define NETDEBUG(...) do { fprintf(stderr, "[NETDEBUG] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#else
#define NETDEBUG(...) do { } while (0)
#endif

typedef struct { unsigned h[8]; unsigned long long bits; unsigned char b[64]; int n; } sha256_ctx;
static const unsigned K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define R(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void sha_block(sha256_ctx *c, const unsigned char *p) {
  unsigned w[64],v[8],t1,t2; int i;
  for(i=0;i<8;i++) v[i]=c->h[i];
  for(i=0;i<16;i++) w[i]=(unsigned)p[i*4]<<24|(unsigned)p[i*4+1]<<16|(unsigned)p[i*4+2]<<8|p[i*4+3];
  for(;i<64;i++){unsigned s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3),s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
  for(i=0;i<64;i++){unsigned S1=R(v[4],6)^R(v[4],11)^R(v[4],25),ch=(v[4]&v[5])^((~v[4])&v[6]),S0=R(v[0],2)^R(v[0],13)^R(v[0],22),maj=(v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]);t1=v[7]+S1+ch+K[i]+w[i];t2=S0+maj;v[7]=v[6];v[6]=v[5];v[5]=v[4];v[4]=v[3]+t1;v[3]=v[2];v[2]=v[1];v[1]=v[0];v[0]=t1+t2;}
  for(i=0;i<8;i++) c->h[i]+=v[i];
}
/* Correct SHA-256 compressor with conventional working variables. */
static void sha256_init(sha256_ctx *c){int i;unsigned h[]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};for(i=0;i<8;i++)c->h[i]=h[i];c->bits=0;c->n=0;}
static void sha256_update(sha256_ctx *c,const void *vp,size_t n){const unsigned char*p=vp;while(n--){c->b[c->n++]=*p++;c->bits+=8;if(c->n==64){sha_block(c,c->b);c->n=0;}}}
static void sha256_final(sha256_ctx *c,unsigned char out[32]){int i;unsigned long long bits=c->bits;c->b[c->n++]=128;while(c->n!=56){if(c->n==64){sha_block(c,c->b);c->n=0;}c->b[c->n++]=0;}for(i=7;i>=0;i--)c->b[c->n++]=(unsigned char)(bits>>(i*8));sha_block(c,c->b);for(i=0;i<8;i++){out[i*4]=c->h[i]>>24;out[i*4+1]=c->h[i]>>16;out[i*4+2]=c->h[i]>>8;out[i*4+3]=c->h[i];}}
static int file_sha256(const char *path,char hex[65]){FILE*f=fopen(path,"rb");unsigned char b[4096],d[32];size_t n;int i;if(!f)return-1;sha256_ctx c;sha256_init(&c);while((n=fread(b,1,sizeof b,f))>0)sha256_update(&c,b,n);fclose(f);sha256_final(&c,d);for(i=0;i<32;i++)sprintf(hex+i*2,"%02x",d[i]);hex[64]=0;return 0;}

static const char *url_path(const char *url){const char*p=strstr(url,"://");if(!p)return NULL;p+=3;p=strchr(p,'/');return p?p:"/";}
static int url_host(const char *url,char *hostbuf,size_t len){const char*p=strstr(url,"://"),*slash;size_t n;if(!p)return-1;p+=3;slash=strchr(p,'/');n=slash?(size_t)(slash-p):strlen(p);if(!n||n>=len)return-1;memcpy(hostbuf,p,n);hostbuf[n]=0;return 0;}
static int http_connect(const char *url){char host[128];int port=80,s;struct hostent*he;struct sockaddr_in sa;char*colon;if(url_host(url,host,sizeof host))return-1;colon=strchr(host,':');if(colon){port=atoi(colon+1);*colon=0;}NETDEBUG("connect URL=%s host=%s port=%d",url,host,port);he=gethostbyname(host);if(!he||!he->h_addr_list[0])return-2;s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return-3;memset(&sa,0,sizeof sa);sa.sin_family=AF_INET;sa.sin_port=htons(port);memcpy(&sa.sin_addr,he->h_addr_list[0],he->h_length);if(connect(s,(struct sockaddr*)&sa,sizeof sa)<0){close(s);return-4;}return s;}
static int http_get_with_range(const char *url,long from,int*code,long*cl,char*expected){const char*path=url_path(url);char host[128],req[512],hdr[4096],range_line[64];int s,rl,sent,n;size_t used=0;char*p;if(!path||url_host(url,host,sizeof host))return-1;s=http_connect(url);if(s<0)return s;if(from)rl=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PSLinker/1.0\r\nRange: bytes=%ld-\r\nConnection: close\r\n\r\n",path,host,from);else rl=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PSLinker/1.0\r\nConnection: close\r\n\r\n",path,host);if(from)snprintf(range_line,sizeof range_line,"bytes=%ld-",from);else snprintf(range_line,sizeof range_line,"none");sent=send(s,req,rl,0);NETDEBUG("GET URL=%s Range=%s result=%d",url,range_line,sent);if(sent!=rl){close(s);return-5;}while(used<sizeof hdr-1){n=recv(s,hdr+used,1,0);if(n<=0){close(s);return-8;}used++;if(used>=4&&!memcmp(hdr+used-4,"\r\n\r\n",4))break;}hdr[used]=0;if(sscanf(hdr,"HTTP/%*d.%*d %d",code)!=1){close(s);return-8;}*cl=-1;p=strcasestr(hdr,"content-length:");if(p)*cl=atol(p+15);expected[0]=0;p=strcasestr(hdr,"x-sha256:");if(!p)p=strcasestr(hdr,"content-sha256:");if(p){p=strchr(p,':');while(p&&*++p==' '){}if(p)sscanf(p,"%64s",expected);}NETDEBUG("HTTP status=%d Range offset=%ld content_length=%ld Accept-Ranges=%s",*code,from,*cl,strcasestr(hdr,"accept-ranges:")?"present":"absent");return s;}
int ps3_resume_download(const char*url,const char*dest_path,resume_progress_cb progress){struct stat st;long from=0,cl,total,got=0;int code,s,append=0,ok=1;FILE*f;char expected[65],actual[65];unsigned char buf[DL_BUF_SIZE];ssize_t n;if(stat(dest_path,&st)==0&&st.st_size>0)from=(long)st.st_size;NETDEBUG("download start URL=%s dest=%s resume_offset=%ld",url,dest_path,from);s=http_get_with_range(url,from,&code,&cl,expected);if(s<0)return s;if(code==206&&from>0){append=1;}else if(code==200){from=0;}else{close(s);return-7;}total=from+(cl>0?cl:0);NETDEBUG("response status=%d mode=%s resume_offset=%ld",code,append?"206 append":"200 restart",from);f=fopen(dest_path,append?"ab":"wb");if(!f){close(s);return-6;}while((n=recv(s,buf,sizeof buf,0))>0){if(fwrite(buf,1,(size_t)n,f)!=(size_t)n){ok=0;break;}from+=n;got+=n;if(progress)progress(total>0?(int)(from*100/total):-1,from);NETDEBUG("bytes written=%ld",from);}fclose(f);close(s);/* A known body length that did not arrive is a truncated transfer, not a
   finished one: never hash and report a short file as complete. */
if(n<0||(cl>0&&got!=cl)){NETDEBUG("download incomplete received=%ld expected=%ld",got,cl);ok=0;}if(!ok)return-9;if(file_sha256(dest_path,actual)!=0)return-10;NETDEBUG("download complete bytes=%ld sha256=%s",from,actual);if(expected[0]&&strcasecmp(expected,actual)){NETDEBUG("SHA-256 mismatch expected=%s actual=%s",expected,actual);return-11;}if(expected[0])NETDEBUG("SHA-256 verified=%s",actual);else NETDEBUG("SHA-256 computed (no response digest to compare)=%s",actual);return 0;}
