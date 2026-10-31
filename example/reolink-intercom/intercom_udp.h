// Sends the panel microphone (PCM s16le 16 kHz mono) as UDP packets to intercom_server.py.
#pragma once
#include <cstring>
#include <lwip/sockets.h>

static int intercom_sock = -1;
static struct sockaddr_in intercom_dst;

inline void intercom_udp_send(const char *host, uint16_t port, const uint8_t *d, size_t n) {
  if (intercom_sock < 0) {
    intercom_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    std::memset(&intercom_dst, 0, sizeof(intercom_dst));
    intercom_dst.sin_family = AF_INET;
    intercom_dst.sin_port = htons(port);
    intercom_dst.sin_addr.s_addr = inet_addr(host);
  }
  if (intercom_sock < 0)
    return;
  // chunks of at most 1024 bytes: one Wi-Fi frame each, no IP fragmentation
  for (size_t o = 0; o < n; o += 1024) {
    size_t k = n - o < 1024 ? n - o : 1024;
    ::sendto(intercom_sock, d + o, k, 0, reinterpret_cast<struct sockaddr *>(&intercom_dst), sizeof(intercom_dst));
  }
}
