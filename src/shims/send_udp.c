/* send_udp -- tiny on-device UDP sender for talk-back testing.
 *   send_udp <ip> <port> tone           -> sends the 8-byte "SPKTONE!" magic
 *   send_udp <ip> <port> file <path>     -> streams <path> as 640-byte S16LE
 *                                           frames, one every 20 ms (raw PCM)
 * Built static musl for MIPSEL; run on the camera. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char **argv){
    if(argc < 4){ fprintf(stderr,"usage: %s <ip> <port> tone|file [path]\n",argv[0]); return 2; }
    const char *ip = argv[1]; int port = atoi(argv[2]);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);

    if(!strcmp(argv[3],"tone")){
        const char *m = "SPKTONE!";
        sendto(s, m, 8, 0, (struct sockaddr*)&a, sizeof a);
        printf("sent SPKTONE! to %s:%d\n", ip, port);
        return 0;
    }
    if(!strcmp(argv[3],"file") && argc>=5){
        FILE *f = fopen(argv[4],"rb");
        if(!f){ perror("fopen"); return 1; }
        unsigned char buf[640]; size_t n; long total=0, frames=0;
        while((n = fread(buf,1,sizeof buf,f)) > 0){
            sendto(s, buf, n, 0, (struct sockaddr*)&a, sizeof a);
            total += (long)n; frames++;
            usleep(20000); /* 20 ms/frame */
        }
        fclose(f);
        printf("streamed %ld bytes in %ld frames to %s:%d\n", total, frames, ip, port);
        return 0;
    }
    fprintf(stderr,"bad args\n"); return 2;
}
