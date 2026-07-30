/* wgctl <ifname> <wg-quick.conf> : create a kernel WireGuard interface and
 * configure it entirely over netlink (rtnetlink for the link, genetlink for the
 * WireGuard SET_DEVICE). Replaces both `ip link add type wireguard` and the
 * 220 KB `wg` tool with one tiny static binary. IPv4 endpoints only.
 *
 * conf keys (section headers ignored): PrivateKey, Address, PublicKey (peer),
 * PresharedKey, Endpoint (host:port), AllowedIPs (a.b.c.d/nn), PersistentKeepalive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>

/* ---- WireGuard uapi (from uapi/linux/wireguard.h) ---- */
#define WG_GENL_NAME "wireguard"
#define WG_GENL_VERSION 1
enum { WG_CMD_SET_DEVICE = 1 };
enum { WGDEVICE_A_IFNAME=2, WGDEVICE_A_PRIVATE_KEY=3, WGDEVICE_A_FLAGS=5,
       WGDEVICE_A_LISTEN_PORT=6, WGDEVICE_A_PEERS=8 };
enum { WGDEVICE_F_REPLACE_PEERS=1 };
enum { WGPEER_A_PUBLIC_KEY=1, WGPEER_A_PRESHARED_KEY=2, WGPEER_A_FLAGS=3,
       WGPEER_A_ENDPOINT=4, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL=5,
       WGPEER_A_ALLOWEDIPS=9 };
enum { WGPEER_F_REPLACE_ALLOWEDIPS=2 };
enum { WGALLOWEDIP_A_FAMILY=1, WGALLOWEDIP_A_IPADDR=2, WGALLOWEDIP_A_CIDR_MASK=3 };
#ifndef NLA_F_NESTED
#define NLA_F_NESTED 0x8000
#endif

static int b64dec(const char *s, unsigned char *out, int outmax){
    static const char *T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val=0,bits=0,n=0;
    for(; *s && *s!='\n' && *s!='\r'; s++){
        if(*s=='=') break;
        const char *p=strchr(T,*s); if(!p) continue;
        val=(val<<6)|(int)(p-T); bits+=6;
        if(bits>=8){ bits-=8; if(n<outmax) out[n++]=(unsigned char)((val>>bits)&0xff); }
    }
    return n;
}

/* --- message builder --- */
static char buf[4096];
static struct nlmsghdr *nlh;
static int put_attr(int type,const void*data,int len){
    struct rtattr *rta=(struct rtattr*)((char*)nlh+NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type=type; rta->rta_len=RTA_LENGTH(len);
    if(len) memcpy(RTA_DATA(rta),data,len);
    nlh->nlmsg_len=NLMSG_ALIGN(nlh->nlmsg_len)+RTA_ALIGN(rta->rta_len);
    return (int)((char*)rta-(char*)nlh);
}
static int nest_start(int type){
    struct rtattr *rta=(struct rtattr*)((char*)nlh+NLMSG_ALIGN(nlh->nlmsg_len));
    int off=(int)((char*)rta-(char*)nlh);
    rta->rta_type=type|NLA_F_NESTED; rta->rta_len=RTA_LENGTH(0);
    nlh->nlmsg_len=NLMSG_ALIGN(nlh->nlmsg_len)+RTA_ALIGN(RTA_LENGTH(0));
    return off;
}
static void nest_end(int off){
    struct rtattr *rta=(struct rtattr*)((char*)nlh+off);
    rta->rta_len=(char*)nlh+nlh->nlmsg_len-(char*)rta;
}

static int nlfd;
static int nl_txrx(int expect_err){
    struct sockaddr_nl sa; memset(&sa,0,sizeof(sa)); sa.nl_family=AF_NETLINK;
    if(send(nlfd,nlh,nlh->nlmsg_len,0)<0){ perror("send"); return -1; }
    char r[8192]; int n=recv(nlfd,r,sizeof(r),0);
    if(n<0){ perror("recv"); return -1; }
    struct nlmsghdr *h=(struct nlmsghdr*)r;
    if(h->nlmsg_type==NLMSG_ERROR){ struct nlmsgerr *e=(struct nlmsgerr*)NLMSG_DATA(h);
        if(e->error && e->error!=-EEXIST){ if(expect_err) fprintf(stderr,"netlink error %d (%s)\n",e->error,strerror(-e->error)); return e->error; } }
    return 0;
}

/* resolve the "wireguard" genl family id via CTRL */
static int wg_family_id(void){
    memset(buf,0,sizeof(buf)); nlh=(struct nlmsghdr*)buf;
    nlh->nlmsg_len=NLMSG_LENGTH(GENL_HDRLEN);
    nlh->nlmsg_type=GENL_ID_CTRL; nlh->nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK;
    struct genlmsghdr *g=(struct genlmsghdr*)NLMSG_DATA(nlh); g->cmd=CTRL_CMD_GETFAMILY; g->version=1;
    put_attr(CTRL_ATTR_FAMILY_NAME, WG_GENL_NAME, strlen(WG_GENL_NAME)+1);
    struct sockaddr_nl sa; memset(&sa,0,sizeof(sa)); sa.nl_family=AF_NETLINK;
    if(send(nlfd,nlh,nlh->nlmsg_len,0)<0){ perror("send ctrl"); return -1; }
    char r[8192]; int n=recv(nlfd,r,sizeof(r),0); if(n<0){ perror("recv ctrl"); return -1; }
    struct nlmsghdr *h=(struct nlmsghdr*)r;
    if(h->nlmsg_type==NLMSG_ERROR) return -1;
    struct genlmsghdr *gg=(struct genlmsghdr*)NLMSG_DATA(h);
    struct rtattr *a=(struct rtattr*)((char*)gg+GENL_HDRLEN);
    int len=h->nlmsg_len-NLMSG_LENGTH(GENL_HDRLEN);
    for(; RTA_OK(a,len); a=RTA_NEXT(a,len))
        if(a->rta_type==CTRL_ATTR_FAMILY_ID) return *(unsigned short*)RTA_DATA(a);
    return -1;
}

/* create the wg netdev via rtnetlink (separate NETLINK_ROUTE socket) */
static int create_iface(const char *ifn){
    int fd=socket(AF_NETLINK,SOCK_RAW,NETLINK_ROUTE); if(fd<0) return -1;
    char b[512]; memset(b,0,sizeof(b));
    struct nlmsghdr *n=(struct nlmsghdr*)b;
    n->nlmsg_len=NLMSG_LENGTH(sizeof(struct ifinfomsg));
    n->nlmsg_flags=NLM_F_REQUEST|NLM_F_CREATE|NLM_F_EXCL|NLM_F_ACK; n->nlmsg_type=RTM_NEWLINK;
    struct nlmsghdr *save=nlh; nlh=n;
    put_attr(IFLA_IFNAME, ifn, strlen(ifn)+1);
    int li=nest_start(IFLA_LINKINFO);
    put_attr(IFLA_INFO_KIND, "wireguard", 10);
    nest_end(li);
    nlh=save;
    struct sockaddr_nl sa; memset(&sa,0,sizeof(sa)); sa.nl_family=AF_NETLINK;
    send(fd,n,n->nlmsg_len,0);
    char r[512]; int rn=recv(fd,r,sizeof(r),0);
    close(fd);
    if(rn>0){ struct nlmsghdr *h=(struct nlmsghdr*)r; if(h->nlmsg_type==NLMSG_ERROR){ struct nlmsgerr *e=(struct nlmsgerr*)NLMSG_DATA(h); if(e->error && e->error!=-EEXIST){ fprintf(stderr,"create %s: %s\n",ifn,strerror(-e->error)); return -1; } } }
    return 0;
}

static char* val(char*hay,const char*key){
    /* case-insensitive line "key = value"; returns malloc'd trimmed value or NULL */
    int kl=strlen(key); char*line=hay;
    while(line&&*line){
        char*eol=strchr(line,'\n'); int llen=eol?(int)(eol-line):(int)strlen(line);
        char*t=line; while(*t==' '||*t=='\t')t++;
        if(strncasecmp(t,key,kl)==0){ char*e=t+kl; while(*e==' '||*e=='\t')e++;
            if(*e=='='){ e++; while(*e==' '||*e=='\t')e++;
                int vl=llen-(int)(e-line); while(vl>0&&(e[vl-1]=='\r'||e[vl-1]==' '||e[vl-1]=='\t'))vl--;
                char*out=malloc(vl+1); memcpy(out,e,vl); out[vl]=0; return out; } }
        line=eol?eol+1:0;
    }
    return NULL;
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: wgctl <ifname> <conf>\n"); return 2; }
    const char *ifn=argv[1];
    FILE*f=fopen(argv[2],"rb"); if(!f){ perror("conf"); return 2; }
    char cfg[8192]; int cl=fread(cfg,1,sizeof(cfg)-1,f); cfg[cl>0?cl:0]=0; fclose(f);

    if(create_iface(ifn)<0) return 3;

    nlfd=socket(AF_NETLINK,SOCK_RAW,NETLINK_GENERIC); if(nlfd<0){ perror("genl socket"); return 3; }
    int fam=wg_family_id(); if(fam<0){ fprintf(stderr,"wireguard genl family not found (module loaded?)\n"); return 3; }

    char *priv=val(cfg,"PrivateKey"), *pub=val(cfg,"PublicKey"), *psk=val(cfg,"PresharedKey");
    char *ep=val(cfg,"Endpoint"), *aips=val(cfg,"AllowedIPs"), *ka=val(cfg,"PersistentKeepalive");
    if(!priv||!pub||!ep){ fprintf(stderr,"conf missing PrivateKey/PublicKey/Endpoint\n"); return 2; }

    memset(buf,0,sizeof(buf)); nlh=(struct nlmsghdr*)buf;
    nlh->nlmsg_len=NLMSG_LENGTH(GENL_HDRLEN); nlh->nlmsg_type=fam; nlh->nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK;
    struct genlmsghdr *g=(struct genlmsghdr*)NLMSG_DATA(nlh); g->cmd=WG_CMD_SET_DEVICE; g->version=WG_GENL_VERSION;
    put_attr(WGDEVICE_A_IFNAME, ifn, strlen(ifn)+1);
    unsigned char key[32];
    if(b64dec(priv,key,32)==32) put_attr(WGDEVICE_A_PRIVATE_KEY,key,32);
    unsigned int dflags=WGDEVICE_F_REPLACE_PEERS; put_attr(WGDEVICE_A_FLAGS,&dflags,4);

    int peers=nest_start(WGDEVICE_A_PEERS);
    int p0=nest_start(0);
    if(b64dec(pub,key,32)==32) put_attr(WGPEER_A_PUBLIC_KEY,key,32);
    if(psk && b64dec(psk,key,32)==32) put_attr(WGPEER_A_PRESHARED_KEY,key,32);
    unsigned int pflags=WGPEER_F_REPLACE_ALLOWEDIPS; put_attr(WGPEER_A_FLAGS,&pflags,4);
    /* endpoint host:port -> sockaddr_in */
    { char h[128]; strncpy(h,ep,sizeof(h)-1); h[sizeof(h)-1]=0; char*c=strrchr(h,':'); int port=51820;
      if(c){ *c=0; port=atoi(c+1); }
      struct sockaddr_in sin; memset(&sin,0,sizeof(sin)); sin.sin_family=AF_INET; sin.sin_port=htons(port);
      if(inet_aton(h,&sin.sin_addr)==0){ fprintf(stderr,"wgctl: endpoint must be an IPv4 address (got '%s'); resolve it at config time\n",h); return 4; }
      put_attr(WGPEER_A_ENDPOINT,&sin,sizeof(sin)); }
    if(ka){ unsigned short k=(unsigned short)atoi(ka); put_attr(WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL,&k,2); }
    /* allowed-ips (comma-separated a.b.c.d/nn) */
    int aip=nest_start(WGPEER_A_ALLOWEDIPS);
    { char list[512]; strncpy(list,aips?aips:"0.0.0.0/0",sizeof(list)-1); list[sizeof(list)-1]=0;
      char*tok=strtok(list,","); int idx=0;
      while(tok){ while(*tok==' ')tok++; char*sl=strchr(tok,'/'); int cidr=32; if(sl){*sl=0; cidr=atoi(sl+1);}
          struct in_addr ia; if(inet_aton(tok,&ia)){ int a1=nest_start(idx++);
              unsigned short fam4=AF_INET; put_attr(WGALLOWEDIP_A_FAMILY,&fam4,2);
              put_attr(WGALLOWEDIP_A_IPADDR,&ia.s_addr,4);
              unsigned char cm=(unsigned char)cidr; put_attr(WGALLOWEDIP_A_CIDR_MASK,&cm,1);
              nest_end(a1); }
          tok=strtok(NULL,","); } }
    nest_end(aip);
    nest_end(p0);
    nest_end(peers);

    int rc=nl_txrx(1);
    if(rc){ fprintf(stderr,"wgctl: SET_DEVICE failed (%d)\n",rc); return 4; }
    printf("wgctl: %s configured\n",ifn);
    return 0;
}
