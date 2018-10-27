/*
 *
 * honggfuzz - Intel PT decoder
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "libhfcommon/common.h"

#include <inttypes.h>
#include <linux/perf_event.h>
#include <stdio.h>

#include "libhfcommon/log.h"
#include "libhfcommon/util.h"
#include "pt.h"

#ifdef _HF_LINUX_INTEL_PT_LIB

#include <intel-pt.h>

struct pt_cpu ptCpu = {
    .vendor = pcv_unknown,
    .family = 0,
    .model = 0,
    .stepping = 0,
};

void perf_ptInit(void) {
    FILE* f = fopen("/proc/cpuinfo", "rb");
    if (!f) {
        PLOG_E("Couldn't open '/proc/cpuinfo'");
    }
    for (;;) {
        char k[1024], t[1024], v[1024];
        int ret = fscanf(f, "%1024[^\t]%1024[\t]: %1024[^\n]\n", k, t, v);
        if (ret == EOF) {
            break;
        }
        if (ret != 3) {
            break;
        }
        LOG_E("'%s' '%s'", k, v);
        if (strcmp(k, "vendor_id") == 0) {
            if (strcmp(v, "GenuineIntel") == 0) {
                ptCpu.vendor = pcv_intel;
                LOG_D("IntelPT vendor: Intel");
            } else {
                ptCpu.vendor = pcv_unknown;
                LOG_D("Current processor is not Intel, IntelPT will not work");
            }
        }
        if (strcmp(k, "cpu family") == 0) {
            ptCpu.family = atoi(v);
            LOG_D("IntelPT family: %" PRIu16, ptCpu.family);
        }
        if (strcmp(k, "model") == 0) {
            ptCpu.model = atoi(v);
            LOG_D("IntelPT model: %" PRIu8, ptCpu.model);
        }
        if (strcmp(k, "stepping") == 0) {
            ptCpu.stepping = atoi(v);
            LOG_D("IntelPT stepping: %" PRIu8, ptCpu.stepping);
        }
    }
    fclose(f);
}

/* Sign-extend a uint64_t value. */
inline static uint64_t sext(uint64_t val, uint8_t sign) {
    uint64_t signbit, mask;

    signbit = 1ull << (sign - 1);
    mask = ~0ull << sign;

    return val & signbit ? val | mask : val & ~mask;
}

__attribute__((hot)) inline static void perf_ptAnalyzePkt(run_t* run, struct pt_packet* packet) {
    if (packet->type != ppt_tip) {
        return;
    }

    uint64_t ip;
    switch (packet->payload.ip.ipc) {
        case pt_ipc_update_16:
            ip = packet->payload.ip.ip & 0xFFFF;
            break;
        case pt_ipc_update_32:
            ip = packet->payload.ip.ip & 0xFFFFFFFF;
            break;
        case pt_ipc_update_48:
            ip = packet->payload.ip.ip & 0xFFFFFFFFFFFF;
            break;
        case pt_ipc_sext_48:
            ip = sext(packet->payload.ip.ip, 48);
            break;
        case pt_ipc_full:
            ip = packet->payload.ip.ip;
            break;
        default:
            return;
    }

    ip &= _HF_PERF_BITMAP_BITSZ_MASK;
    register uint8_t prev = ATOMIC_BTS(run->global->feedback.feedbackMap->bbMapPc, ip);
    if (!prev) {
        run->linux.hwCnts.newBBCnt++;
    }
    return;
}

void arch_ptAnalyze(run_t* run) {
    struct perf_event_mmap_page* pem = (struct perf_event_mmap_page*)run->linux.perfMmapBuf;

    uint64_t aux_tail = ATOMIC_GET(pem->aux_tail);
    uint64_t aux_head = ATOMIC_GET(pem->aux_head);

    /* smp_rmb() required as per /usr/include/linux/perf_event.h */
    rmb();

    struct pt_config ptc;
    pt_config_init(&ptc);
    ptc.begin = &run->linux.perfMmapAux[aux_tail];
    ptc.end = &run->linux.perfMmapAux[aux_head - 1];
    ptc.cpu = ptCpu;

    int errcode = pt_cpu_errata(&ptc.errata, &ptc.cpu);
    if (errcode < 0) {
        LOG_F("pt_errata() failed: %s", pt_errstr(-errcode));
    }

    struct pt_packet_decoder* ptd = pt_pkt_alloc_decoder(&ptc);
    if (ptd == NULL) {
        LOG_F("pt_pkt_alloc_decoder() failed");
    }
    defer {
        pt_pkt_free_decoder(ptd);
    };

    errcode = pt_pkt_sync_forward(ptd);
    if (errcode < 0) {
        LOG_W("pt_pkt_sync_forward() failed: %s", pt_errstr(-errcode));
        return;
    }

    for (;;) {
        struct pt_packet packet;
        errcode = pt_pkt_next(ptd, &packet, sizeof(packet));
        if (errcode == -pte_eos) {
            break;
        }
        if (errcode < 0) {
            LOG_W("pt_pkt_next() failed: %s", pt_errstr(-errcode));
            break;
        }
        perf_ptAnalyzePkt(run, &packet);
    }
}

#else /* _HF_LINUX_INTEL_PT_LIB */

void arch_ptAnalyze(run_t* fuzzer HF_ATTR_UNUSED) {
    LOG_F(
        "The program has not been linked against the Intel's Processor Trace Library (libipt.so)");
}

#endif /* _HF_LINUX_INTEL_PT_LIB */
