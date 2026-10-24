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

// Loopback port that ffplay listens on (ffplay -f s16le -ar 48000 -i udp://127.0.0.1:PLAYBACK_PORT).
// Forwarding decoded PCM over a socket instead of stdout sidesteps Windows'
// CRT text-mode newline translation on piped stdout, which would otherwise
// corrupt any 0x0A byte in the raw audio stream.
#define PLAYBACK_PORT 5555

#pragma pack(push, 1)

typedef struct{
    uint8_t vpxcc;
    uint8_t m_pt;
    uint16_t seq_num;
    uint32_t timestamp;
    uint32_t ssrc;
} RTPHeader;

#pragma pack(pop)

int main(){
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

    int playback_socket;
    struct sockaddr_in playback_addr = {.sin_family = AF_INET, .sin_port = htons(PLAYBACK_PORT)};
    playback_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if((playback_socket = socket(AF_INET, SOCK_DGRAM, 0)) <= 0){
        perror("Failed to create playback socket");
        return -1;
    }

    int err;

    socklen_t address_length = sizeof(peer_addr);
    OpusDecoder *dec = opus_decoder_create(48000, 1, &err);
    unsigned char buf[4000];
    int16_t pcm[960];
    RTPHeader rtp;
    uint16_t seq_num;
    uint16_t expected_seq;
    uint32_t timestamp;

    int16_t gap;

    int first = 1;

    int samples;

    while (1) {
        int n = recvfrom(udp_rx_socket, buf, sizeof(buf), 0, (struct sockaddr*)&peer_addr, &address_length);
        if (n <= 0) {
            continue;
        }

        memcpy(&rtp, buf, 12);
        seq_num = ntohs(rtp.seq_num);
        if(first){ expected_seq = seq_num; first = 0; }
        timestamp = ntohl(rtp.timestamp);

        gap = (int16_t)(seq_num - expected_seq);

        if(gap > 200 || gap < -200){
            printf("Resyncing gap.. \n");
            gap = 0;
        }

        if(gap > 0){ //1 or more packets lost
            printf("%d packet(s) lost\n", gap);
            for(int i = 0; i < gap; i++){
                samples = opus_decode(dec, NULL, 0, pcm, 960, 0);
                if (samples < 0) {
                    fprintf(stderr, "opus_decode error: %s\n", opus_strerror(samples));
                    continue;
                }
                sendto(playback_socket, (const char*)pcm, samples * sizeof(int16_t), 0, (struct sockaddr*)&playback_addr, sizeof(playback_addr));
            }
        } else if(gap < 0){
            continue;
        }
        
        expected_seq = seq_num + 1;

        samples = opus_decode(dec, buf + 12, n - 12, pcm, 960, 0);
        if (samples < 0) {
            fprintf(stderr, "opus_decode error: %s\n", opus_strerror(samples));
            continue;
        }

        sendto(playback_socket, (const char*)pcm, samples * sizeof(int16_t), 0,
               (struct sockaddr*)&playback_addr, sizeof(playback_addr));
    }

    closesocket(udp_rx_socket);
    closesocket(playback_socket);

#ifdef _WIN32
    WSACleanup();
#endif
}