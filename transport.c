// Tomasz Stachurski 322414

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/ip.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>

#define SWS 1000                  // sender window size
#define TIMEOUT_RETRY 0.5         // timeout to retry sending a request
#define REQUEST_SIZE 1000         // maximum size of requested packet
#define TIMEOUT 1000              // 1ms

struct segment {
    uint32_t frame;               // frame number
    bool ACK;                     // received and accepted
    struct timeval last_send;     // last request sent
    char packet[REQUEST_SIZE];    // data
};

struct segment segments[SWS];
int window_limit = SWS;
uint32_t LAR = -1;                // last ACK received

void parse_input(char **argv, struct sockaddr_in *recipient, FILE **file, uint32_t *size, int *sockfd) {
    uint32_t port = atoi(argv[2]);
    if (port > 65535) {
        fprintf(stderr, "%d is not a valid port!\n", port);
        exit(EXIT_FAILURE);
    }

    *file = fopen(argv[3], "wb");
    if (file == NULL) {
        fprintf(stderr, "Opening error for file: %s\n", argv[3]);
        exit(EXIT_FAILURE);
    }

    *size = atoi(argv[4]);
    if (*size > 10000000) {
        fprintf(stderr, "%s is not a valid size!\n", argv[4]);
        exit(EXIT_FAILURE);
    }

    memset(recipient, 0, sizeof(*recipient));
    int status = inet_pton(AF_INET, argv[1], &(recipient->sin_addr));
    if (status == 0) {
        fprintf(stderr, "%s is not a valid IPv4 adress!\n", argv[1]);
        exit(EXIT_FAILURE);
    } else if (status < 0) {
        fprintf(stderr, "inet_pton error!\n");
        exit(EXIT_FAILURE);
    }

    *sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (*sockfd < 0) {
        fprintf(stderr, "socket error!\n");
        exit(EXIT_FAILURE);
    }
    recipient->sin_family = AF_INET;
    recipient->sin_port = htons(port);
}

double time_elapsed(int i) {
    struct timeval t1, t2;
    t1 = segments[i % SWS].last_send;
    gettimeofday(&t2, NULL);

    double elapsed = (t2.tv_sec - t1.tv_sec) * 1000;    // s -> ms 
    elapsed += (t2.tv_usec - t1.tv_usec) / 1000;        // us -> ms

    return elapsed;
}

bool retry_segment(int i, uint32_t seg_cnt) {
    if (segments[i % SWS].ACK) {
        return false;
    }
    if ((uint32_t)segments[i].frame >= seg_cnt) {
        return false;
    }

    return time_elapsed(i) >= TIMEOUT_RETRY;
}

uint32_t get_seg_size(int i, uint32_t total) {
    uint32_t len = segments[i].frame * REQUEST_SIZE;

    if ((len + REQUEST_SIZE) > total) {
        return total - len;
    }
    else {
        return REQUEST_SIZE;
    }
}

void send_request(int sockfd, struct sockaddr_in *recipient, uint32_t start, uint32_t len) {
    char message[20];
    int msg = sprintf(message, "GET %u %u\n", start, len);
    if (msg < 0) {
        fprintf(stderr, "sprintf error!\n");
        exit(EXIT_FAILURE);
    }

    size_t sent = sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)recipient, sizeof(*recipient));
    if (sent != (strlen(message))) {
        fprintf(stderr, "sendto sent too few bytes!\n");
        exit(EXIT_FAILURE);
    }
}

void send_segment(int i, int sockfd, struct sockaddr_in *recipient, uint32_t size) {
    uint32_t start = i * REQUEST_SIZE;
    uint32_t len = REQUEST_SIZE;

    // if last packet is smaller
    if (start + len > size) {
        len = size - start;
    }

    // update time and send
    gettimeofday(&segments[i % SWS].last_send, NULL);
    send_request(sockfd, recipient, start, len);
}

void retry_send_all(int sockfd, struct sockaddr_in *recipient, uint32_t size, uint32_t LAR) {
    for (int i=0; i < window_limit; i++) {
        int j = i + LAR + 1;
        if(retry_segment(j % window_limit, ceil((double)size / (double)REQUEST_SIZE))) {
            send_segment(j, sockfd, recipient, size);
        }
    }
}

bool receive_packet(int sockfd, struct sockaddr_in *recipient) {
    struct sockaddr_in sender;
    char buffer[IP_MAXPACKET];

    socklen_t sender_size = sizeof(sender);
    ssize_t packet_length = recvfrom(sockfd, buffer, IP_MAXPACKET, 0, (struct sockaddr *)&sender, &sender_size); // lepiej zrobic connecta
    if (packet_length < 0) {
        fprintf(stderr, "recvfrom error!\n");
        exit(EXIT_FAILURE);
    }

    if (recipient->sin_addr.s_addr != sender.sin_addr.s_addr || recipient->sin_port != sender.sin_port) {
        return false;
    }

    uint32_t start, size;
    int msg = sscanf(buffer, "DATA %u %u\n", &start, &size);
    // message error
    if (msg != 2) {
        return false;
    }

    size_t segment_id = (start / REQUEST_SIZE) % SWS;

    // IF already received, incorrect size,
    // old packet, packet too far THEN leave
    if (segments[segment_id].ACK                        ||
    start != segments[segment_id].frame * REQUEST_SIZE  ||
    (LAR + 1) * REQUEST_SIZE > start                    ||
    start > (LAR + 1 + SWS) * REQUEST_SIZE) {
        return false;
    }

    // move pointer past '\n'
    size_t data = 0;
    while (buffer[data++] != '\n');

    memcpy(segments[segment_id].packet, &buffer[data], size);
    segments[segment_id].ACK = true;
    
    return true;
}

void receive_all(int sockfd, struct sockaddr_in *recipient) {
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT;

    fd_set desc;
    FD_ZERO(&desc);
    FD_SET(sockfd, &desc);

    int ready;
    while ((ready = select(sockfd + 1, &desc, NULL, NULL, &timeout)) > 0) {
        if (ready > 0) {
            receive_packet(sockfd, recipient);
        }
    }
}

void move_window(uint32_t *LAR, uint32_t seg_cnt, uint32_t *saved_bytes, uint32_t size, FILE **file) {
    size_t index = (*LAR + 1) % SWS;

    while (index < seg_cnt && segments[index].ACK) {
        uint32_t seg_size = get_seg_size(index, size);
        size_t written = fwrite(segments[index].packet, sizeof(char), seg_size, *file);
        if (written != seg_size) {
            fprintf(stderr, "fwrite error!\n");
            exit(EXIT_FAILURE);
        }
        *saved_bytes += seg_size;

        // free slot for new segment
        segments[index].ACK = false;
        segments[index].frame += SWS;

        *LAR += 1;
        index = (*LAR + 1) % SWS;
    }
}

int main(int argc, char **argv) {
    if (argc != 5) {
        printf("Usage: ./transport IP PORT FILE SIZE");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in recipient;
    FILE *file;
    uint32_t size;
    int sockfd;

    parse_input(argv, &recipient, &file, &size, &sockfd);

    // how many segments needed 
    uint32_t seg_cnt = ceil((double)size / (double)REQUEST_SIZE);
    uint32_t saved_bytes = 0;

    // file size smaller than total window size
    if (SWS * REQUEST_SIZE > size) {
        window_limit = ceil((double)size / REQUEST_SIZE); // lepiej (size + request_size - 1) / request_size
    }

    // initialize segments and send requests
    for (int i=0; i < window_limit; i++) {
        segments[i].ACK = false;
        segments[i].frame = i;
        send_segment(i, sockfd, &recipient, size);
    }

    while (LAR + 1 < seg_cnt) {
        printf("PROGRESS: %.2f%%\r", ((double)saved_bytes / size) * 100);
        receive_all(sockfd, &recipient);
        move_window(&LAR, seg_cnt, &saved_bytes, size, &file);
        retry_send_all(sockfd, &recipient, size, LAR);
    }

    fclose(file);
    close(sockfd);
}