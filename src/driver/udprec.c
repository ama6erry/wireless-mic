#include <stdio.h>
#include <strings.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT 9800

int main(){
    char buffer[128];

    int udp_rx_socket;
    struct sockaddr_in peer_addr;
    struct sockaddr_in my_addr = {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT)};

    if((udp_rx_socket = socket(AF_INET, SOCK_DGRAM, 0)) <= 0){
        perror("Failed to create socket");
        return -1;
    }

    int result = bind(udp_rx_socket, &my_addr, sizeof(my_addr));

    if(result < 0){
        perror("Failed to bind socket");
        return -1;
    }

    socklen_t address_length = sizeof(peer_addr);
    int bytes_received = recvfrom(udp_rx_socket, buffer, 128, 0, &peer_addr, &address_length);

    printf("Received a packet from %s:%d -- Message = %s\n", inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port), buffer);

    close(udp_rx_socket);

}