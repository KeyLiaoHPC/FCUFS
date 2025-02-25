#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/unistd.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include "power_reader.h"

struct read_format {
  uint64_t nr;
  struct {
    uint64_t value;
    uint64_t id;
  } values[];
};

void do_something() {
  int i;
  char* ptr;

  ptr = malloc(100*1024*1024);
  for (i = 0; i < 100*1024*1024; i++) {
    ptr[i] = (char) (i & 0xff); // pagefault
  }
  free(ptr);
}

struct EVENT {
    char name[1024];
    uint64_t event_code;
    uint64_t umask_code;
};

// UOPS_RETIRED.ALL: 0xC2, 0x01
// BR_INST_RETIRED.ALL_BRANCHES: 0xC4, 0x00
// BR_MISP_RETIRED.ALL_BRANCHES: 0xC5, 0x00
// MEM_INST_RETIRED.ALL_LOADS: 0xD0, 0x81
// MEM_INST_RETIRED.ALL_STORES: 0xD0, 0x82
// LONGEST_LAT_CACHE.REFERENCE: 0x2E, 0x4F
// LONGEST_LAT_CACHE.MISS: 0x2E, 0x41
// MEM_LOAD_UOPS_RETIRED.LLC_MISS: 0xD1, 0x20
// RESOURCE_STALLS.ANY: 0xA2, 0x01
// CYCLE_ACTIVITY.STALLS_TOTAL: 0xA3, 0x04
const int event_num = 6;
uint64_t measure_val[128][6];
uint32_t time_slice = 500 * 1000; //us
static const struct EVENT event_list[] = {
    {"cpu_cycle", 0x00, 0x01},
    {"uop_num", 0xC2, 0x01},
    {"uop_load_num", 0xD0, 0x81},
    {"uop_store_num", 0xD0, 0x82},
    {"core_L3_ref_num", 0x2E, 0x4F},
    {"core_L3_mis_num", 0x2E, 0x41},
};

uint64_t pfcCreateCfg(uint64_t evtNum, uint64_t umaskVal) {
	uint64_t cfg = (0ULL        << 24)  |
	               (0ULL        << 23)  |
	               (1ULL        << 22)  |
	               (0ULL        << 21)  |
	               (0ULL        << 18)  |
	               (0ULL        << 17)  |
	               (1ULL        << 16)  |
	               (umaskVal    <<  8)  |
	               (evtNum      <<  0);
	return cfg;
}

void *moniter() {
    const int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    power_reader_init(2);
    struct perf_event_attr pea;
    int fd1[num_cores], fd2[num_cores];
    uint64_t id[event_num];
    uint64_t s_val[num_cores][event_num];
    char buf[4096];
    struct read_format* rf = (struct read_format*) buf;
    int i;

    memset(&pea, 0, sizeof(struct perf_event_attr));
    pea.type = PERF_TYPE_HARDWARE;
    pea.size = sizeof(struct perf_event_attr);
    pea.config = PERF_COUNT_HW_CPU_CYCLES;
    pea.disabled = 1;
    pea.exclude_kernel = 1;
    pea.exclude_hv = 1;
    pea.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    for (int i = 0; i < num_cores; ++i) {
        fd1[i] = syscall(__NR_perf_event_open, &pea, -1, i, -1, 0);
        if (fd1[i] == -1) {
            printf("it occurs error when call __NR_perf_event_open\n");
            exit(-1);
        }
        //ioctl(fd1[i], PERF_EVENT_IOC_ID, &id[0]);
    }

    for (int j = 0; j < num_cores; ++j) {
        for (i = 1; i < event_num; ++i) {
            memset(&pea, 0, sizeof(struct perf_event_attr));
            pea.type = PERF_TYPE_RAW;
            pea.size = sizeof(struct perf_event_attr);
            pea.config = pfcCreateCfg(event_list[i].event_code, event_list[i].umask_code);
            pea.disabled = 1;
            pea.exclude_kernel = 1;
            pea.exclude_hv = 1;
            pea.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
            fd2[j] = syscall(__NR_perf_event_open, &pea, -1, j, fd1[j], 0);
            if (fd2[j] == -1) {
                printf("it occurs error when call __NR_perf_event_open\n");
                exit(-1);
            }
            //ioctl(fd2[j], PERF_EVENT_IOC_ID, &id[i]);   
        }
        ioctl(fd1[j], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(fd1[j], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }

    for (int j = 0; j < num_cores; ++j) {
        read(fd1[j], buf, sizeof(buf));
        for (i = 0; i < rf->nr; i++) {
            s_val[j][i] = rf->values[i].value;
        }
    }

    while(1) {
        power_reader_start();

        usleep(time_slice);

        for (int j = 0; j < num_cores; ++j) {
            read(fd1[j], buf, sizeof(buf));
            for (int i = 0; i < rf->nr; i++) {
                measure_val[j][i] = rf->values[i].value - s_val[j][i];
                s_val[j][i] = rf->values[i].value;
            }
        }
        uint64_t energy = power_reader_end();

        for (int i = 0; i < event_num; ++i) {
            for (int j = 0; j < num_cores; ++j) {
                printf("%s:%llu,", event_list[i].name, measure_val[j][i]);
            }
        }
        printf("power:%llu\n", energy);
        fflush(stdout);
    }

    for (int j = 0; j < num_cores; ++j) {
        ioctl(fd1[j], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        time_slice = atoi(argv[1]);
    }
    moniter();

    return 0;
}
