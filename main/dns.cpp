#include "dns.hpp"

#include <cstring>

#include "config.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "dns";

static void dnsTask(void*) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) { ESP_LOGE(TAG, "socket"); vTaskDelete(nullptr); }
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(53);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (sockaddr*)&a, sizeof a) < 0) { ESP_LOGE(TAG, "bind"); close(s); vTaskDelete(nullptr); }

  uint8_t buf[512];
  while (true) {
    sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(s, buf, 400, 0, (sockaddr*)&from, &fl);   // 400: оставляем место под ответ
    if (n < 12 || (buf[2] & 0x80)) continue;                    // не запрос
    // конец секции вопроса: QNAME, затем QTYPE и QCLASS
    int p = 12;
    while (p < n && buf[p]) p += buf[p] + 1;
    p += 5;
    if (p > n) continue;
    uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];
    int len = p;
    buf[2] = 0x81;               // QR=1, RD=1
    buf[3] = 0x80;               // RA=1, RCODE=0
    buf[6] = buf[7] = 0;         // ANCOUNT
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    if (qtype == 1) {            // A -> адрес AP; остальные типы получают пустой NOERROR
      static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
      memcpy(buf + len, ans, sizeof ans);
      len += sizeof ans;
      buf[7] = 1;
    }
    sendto(s, buf, len, 0, (sockaddr*)&from, fl);
  }
}

void dnsStart() {
  xTaskCreate(dnsTask, "dns", 3072, nullptr, 3, nullptr);
}
