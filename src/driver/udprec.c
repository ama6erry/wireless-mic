#include <stdio.h>
#include <opus/opus.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <strings.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define closesocket close
#endif

#define PORT 9800

int main(){
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return -1;
    }
#endif

    int udp_rx_socket;
    struct sockaddr_in peer_addr;
    struct sockaddr_in my_addr = {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT)};

    if((udp_rx_socket = socket(AF_INET, SOCK_DGRAM, 0)) <= 0){
        perror("Failed to create socket");
        return -1;
    }

    int result = bind(udp_rx_socket, (const struct sockaddr *) &my_addr, sizeof(my_addr));

    if(result < 0){
        perror("Failed to bind socket");
        return -1;
    }

    int err;

    socklen_t address_length = sizeof(peer_addr);
    OpusDecoder *dec = opus_decoder_create(48000, 1, &err);
    unsigned char buf[4000];
    int16_t pcm[960];

    while (1) {
        int n = recvfrom(udp_rx_socket, buf, sizeof(buf), 0, (struct sockaddr*)&peer_addr, &address_length);
        if (n <= 0) {
            continue;
        }

        int samples = opus_decode(dec, buf, n, pcm, 960, 0);
        if (samples < 0) {
            fprintf(stderr, "opus_decode error: %s\n", opus_strerror(samples));
            continue;
        }

        fwrite(pcm, sizeof(int16_t), samples, stdout);
    }

    closesocket(udp_rx_socket);

#ifdef _WIN32
    WSACleanup();
#endif
}