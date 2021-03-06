#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include "lpm.h"

#include <klee/klee.h>
#include <castan/scenario.h>

#define LONGEST_PREFIX 24

struct {
  data_t data;
  char prefix_len;
} * prefix_map;

void lpm_init() {
  prefix_map = calloc(1 << LONGEST_PREFIX, sizeof(*prefix_map));
}

void lpm_set_prefix_data(struct in_addr *ip, int prefix_len, data_t data) {
  if (prefix_len > LONGEST_PREFIX) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, ip, ip_str, sizeof(ip_str));
    fprintf(stderr, "Prefix %s/%d is too long. Ignoring.\n", ip_str,
            prefix_len);
    return;
  }

#ifdef __clang__
  for (int i = 0; i < 1 << (LONGEST_PREFIX - prefix_len); i++) {
    if (prefix_map[i].prefix_len <= prefix_len) {
      prefix_map[i].data = data;
      prefix_map[i].prefix_len = prefix_len;
    }
  }
#else
  for (int i =
           (ip->s_addr & ~((1 << (sizeof(ip->s_addr) * 8 - prefix_len)) - 1)) >>
           (sizeof(ip->s_addr) * 8 - LONGEST_PREFIX);
       i <= (ip->s_addr | ((1 << (sizeof(ip->s_addr) * 8 - prefix_len)) - 1)) >>
       (sizeof(ip->s_addr) * 8 - LONGEST_PREFIX);
       i++) {
    if (prefix_map[i].prefix_len <= prefix_len) {
      prefix_map[i].data = data;
      prefix_map[i].prefix_len = prefix_len;
    }
  }
#endif
}

data_t lpm_get_ip_data(struct in_addr *ip) {
#ifdef __clang__
  if (scenario == BEST_CASE) {
    return prefix_map[0].data;
  } else {
    static int i = 0;
    i += 64;
    if (i > sizeof(prefix_map) / sizeof(prefix_map[0])) {
      i = 0;
    }
    return prefix_map[i].data;
  }
#else
  return prefix_map[ip->s_addr >> (sizeof(ip->s_addr) * 8 - LONGEST_PREFIX)]
      .data;
#endif
}
