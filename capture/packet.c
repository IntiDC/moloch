/* packet.c  -- Functions for acquiring data
 *
 * Copyright 2012-2017 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "moloch.h"
#include "patricia.h"
#include <inttypes.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <errno.h>

//#define DEBUG_PACKET

/******************************************************************************/
extern MolochConfig_t        config;

MolochPcapFileHdr_t          pcapFileHeader;

uint64_t                     totalPackets;
LOCAL uint64_t               totalBytes[MOLOCH_MAX_PACKET_THREADS];

LOCAL uint32_t               initialDropped = 0;
struct timeval               initialPacket; // Don't make LOCAL for now because of netflow plugin

extern void                 *esServer;
extern uint32_t              pluginsCbs;

uint64_t                     writtenBytes;
uint64_t                     unwrittenBytes;

LOCAL int                    mac1Field;
LOCAL int                    mac2Field;
LOCAL int                    oui1Field;
LOCAL int                    oui2Field;
LOCAL int                    vlanField;
LOCAL int                    greIpField;

LOCAL uint64_t               droppedFrags;

time_t                       currentTime[MOLOCH_MAX_PACKET_THREADS];
time_t                       lastPacketSecs[MOLOCH_MAX_PACKET_THREADS];
LOCAL int                    inProgress[MOLOCH_MAX_PACKET_THREADS];

LOCAL patricia_tree_t       *ipTree4 = 0;
LOCAL patricia_tree_t       *ipTree6 = 0;

extern MolochFieldOps_t      readerFieldOps[256];

LOCAL MolochPacketEnqueue_cb ethernetCbs[0x10000];
LOCAL MolochPacketEnqueue_cb ipCbs[0x110];

int tcpMProtocol;
int udpMProtocol;

LOCAL int                    mProtocolCnt;
MolochProtocol_t             mProtocols[0x100];

/******************************************************************************/

LOCAL uint64_t               packetStats[MOLOCH_PACKET_MAX];

/******************************************************************************/
LOCAL  MolochPacketHead_t    packetQ[MOLOCH_MAX_PACKET_THREADS];
LOCAL  uint32_t              overloadDrops[MOLOCH_MAX_PACKET_THREADS];

LOCAL  MOLOCH_LOCK_DEFINE(frags);

LOCAL int moloch_packet_ip4(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len);
LOCAL int moloch_packet_ip6(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len);
LOCAL int moloch_packet_frame_relay(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len);
LOCAL int moloch_packet_ether(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len);

typedef struct molochfrags_t {
    struct molochfrags_t  *fragh_next, *fragh_prev;
    struct molochfrags_t  *fragl_next, *fragl_prev;
    uint32_t               fragh_bucket;
    uint32_t               fragh_hash;
    MolochPacketHead_t     packets;
    char                   key[10];
    uint32_t               secs;
    char                   haveNoFlags;
} MolochFrags_t;

typedef struct {
    struct molochfrags_t  *fragh_next, *fragh_prev;
    struct molochfrags_t  *fragl_next, *fragl_prev;
    short                  fragh_bucket;
    uint32_t               fragh_count;
    uint32_t               fragl_count;
} MolochFragsHead_t;

typedef HASH_VAR(h_, MolochFragsHash_t, MolochFragsHead_t, 199337);

LOCAL MolochFragsHash_t          fragsHash;
LOCAL MolochFragsHead_t          fragsList;

// These are in network byte order
LOCAL MolochDropHashGroup_t      packetDrop4;
LOCAL MolochDropHashGroup_t      packetDrop6;

#ifndef IPPROTO_IPV4
#define IPPROTO_IPV4            4
#endif

/******************************************************************************/
void moloch_packet_free(MolochPacket_t *packet)
{
    if (packet->copied) {
        free(packet->pkt);
    }
    packet->pkt = 0;
    MOLOCH_TYPE_FREE(MolochPacket_t, packet);
}
/******************************************************************************/
void moloch_packet_process_data(MolochSession_t *session, const uint8_t *data, int len, int which)
{
    int i;

    for (i = 0; i < session->parserNum; i++) {
        if (session->parserInfo[i].parserFunc) {
            int consumed = session->parserInfo[i].parserFunc(session, session->parserInfo[i].uw, data, len, which);
            if (consumed) {
                session->consumed[which] += consumed;
            }

            if (consumed >= len)
                break;
        }
    }
}
/******************************************************************************/
void moloch_packet_thread_wake(int thread)
{
    MOLOCH_LOCK(packetQ[thread].lock);
    MOLOCH_COND_SIGNAL(packetQ[thread].lock);
    MOLOCH_UNLOCK(packetQ[thread].lock);
}
/******************************************************************************/
/* Only called on main thread, we busy block until all packet threads are empty.
 * Should only be used by tests and at end
 */
void moloch_packet_flush()
{
    int flushed = 0;
    int t;
    while (!flushed) {
        flushed = !moloch_session_cmd_outstanding();

        for (t = 0; t < config.packetThreads; t++) {
            MOLOCH_LOCK(packetQ[t].lock);
            if (DLL_COUNT(packet_, &packetQ[t]) > 0) {
                flushed = 0;
            }
            MOLOCH_UNLOCK(packetQ[t].lock);
            usleep(10000);
        }
    }
}
/******************************************************************************/
SUPPRESS_ALIGNMENT
LOCAL void moloch_packet_process(MolochPacket_t *packet, int thread)
{
#ifdef DEBUG_PACKET
    LOG("Processing %p %d", packet, packet->pktlen);
#endif

    lastPacketSecs[thread] = packet->ts.tv_sec;

    moloch_pq_run(thread, 10);

    MolochSession_t     *session;
    struct ip           *ip4 = (struct ip*)(packet->pkt + packet->ipOffset);
    struct ip6_hdr      *ip6 = (struct ip6_hdr*)(packet->pkt + packet->ipOffset);
    char                 sessionId[MOLOCH_SESSIONID_LEN];


    mProtocols[packet->mProtocol].createSessionId(sessionId, packet);

    int isNew;
    session = moloch_session_find_or_create(packet->mProtocol, packet->hash, sessionId, &isNew);

    if (isNew) {
        session->saveTime = packet->ts.tv_sec + config.tcpSaveTimeout;
        session->firstPacket = packet->ts;
        session->thread = thread;

        if (packet->ipProtocol) {
            session->ipProtocol = packet->ipProtocol;
            if (ip4->ip_v == 4) {
                ((uint32_t *)session->addr1.s6_addr)[2] = htonl(0xffff);
                ((uint32_t *)session->addr1.s6_addr)[3] = ip4->ip_src.s_addr;
                ((uint32_t *)session->addr2.s6_addr)[2] = htonl(0xffff);
                ((uint32_t *)session->addr2.s6_addr)[3] = ip4->ip_dst.s_addr;
                session->ip_tos = ip4->ip_tos;
            } else {
                session->addr1 = ip6->ip6_src;
                session->addr2 = ip6->ip6_dst;
                session->ip_tos = 0;
            }
        }
    }

    mProtocols[packet->mProtocol].preProcess(session, packet, isNew);

    if (session->stopSPI) {
        moloch_packet_free(packet);
        return;
    }

    if (isNew) {
        moloch_parsers_initial_tag(session);
        if (readerFieldOps[packet->readerPos].num)
            moloch_field_ops_run(session, &readerFieldOps[packet->readerPos]);

        if (pluginsCbs & MOLOCH_PLUGIN_NEW)
            moloch_plugins_cb_new(session);

        moloch_rules_session_create(session);
    }

    /* Check if the stop saving bpf filters match */
    if (session->packets[packet->direction] == 0 && session->stopSaving == 0xffff) {
        moloch_rules_run_session_setup(session, packet);
    }

    session->packets[packet->direction]++;
    session->bytes[packet->direction] += packet->pktlen;
    session->lastPacket = packet->ts;

    uint32_t packets = session->packets[0] + session->packets[1];

    if (packets <= session->stopSaving) {
        moloch_writer_write(session, packet);

        // If writerFilePos is 0, then the writer couldn't save the packet
        if (packet->writerFilePos == 0) {
            if (!session->diskOverload) {
                moloch_session_add_tag(session, "pcap-disk-overload");
                session->diskOverload = 1;
            }
            MOLOCH_THREAD_INCR_NUM(unwrittenBytes, packet->pktlen);
        } else {

            MOLOCH_THREAD_INCR_NUM(writtenBytes, packet->pktlen);

            // If the last fileNum used in the session isn't the same as the
            // lastest packets fileNum then we need to add to the filePos and
            // fileNum arrays.
            int16_t len;
            if (session->lastFileNum != packet->writerFileNum) {
                session->lastFileNum = packet->writerFileNum;
                g_array_append_val(session->fileNumArray, packet->writerFileNum);
                int64_t pos = -1LL * packet->writerFileNum;
                g_array_append_val(session->filePosArray, pos);

                if (config.enablePacketLen) {
                    len = 0;
                    g_array_append_val(session->fileLenArray, len);
                }
            }

            g_array_append_val(session->filePosArray, packet->writerFilePos);

            if (config.enablePacketLen) {
                len = 16 + packet->pktlen;
                g_array_append_val(session->fileLenArray, len);
            }
        }

        if (packets >= config.maxPackets || session->midSave) {
            moloch_session_mid_save(session, packet->ts.tv_sec);
        }
    } else {
        // If we hit stopSaving for this session and try and save 1 more packet then
        // add truncated-pcap tag to the session
        if (packets - 1 == session->stopSaving) {
            moloch_session_add_tag(session, "truncated-pcap");
        }
        MOLOCH_THREAD_INCR_NUM(unwrittenBytes, packet->pktlen);
    }

    if (session->firstBytesLen[packet->direction] < 8 && session->packets[packet->direction] < 10) {
        const uint8_t *pcapData = packet->pkt;

        if (pcapFileHeader.linktype == 1) {
            if (packet->direction == 1) {
                moloch_field_macoui_add(session, mac1Field, oui1Field, pcapData+0);
                moloch_field_macoui_add(session, mac2Field, oui2Field, pcapData+6);
            } else {
                moloch_field_macoui_add(session, mac1Field, oui1Field, pcapData+6);
                moloch_field_macoui_add(session, mac2Field, oui2Field, pcapData+0);
            }

            int n = 12;
            while (pcapData[n] == 0x81 && pcapData[n+1] == 0x00) {
                uint16_t vlan = ((uint16_t)(pcapData[n+2] << 8 | pcapData[n+3])) & 0xfff;
                moloch_field_int_add(vlanField, session, vlan);
                n += 4;
            }
        }

        if (packet->vlan)
            moloch_field_int_add(vlanField, session, packet->vlan);

        if (packet->tunnel & MOLOCH_PACKET_TUNNEL_GRE) {
            ip4 = (struct ip*)(packet->pkt + packet->vpnIpOffset);
            moloch_field_ip4_add(greIpField, session, ip4->ip_src.s_addr);
            moloch_field_ip4_add(greIpField, session, ip4->ip_dst.s_addr);
            moloch_session_add_protocol(session, "gre");
        }

        if (packet->tunnel & MOLOCH_PACKET_TUNNEL_PPPOE) {
            moloch_session_add_protocol(session, "pppoe");
        }

        if (packet->tunnel & MOLOCH_PACKET_TUNNEL_PPP) {
            moloch_session_add_protocol(session, "ppp");
        }

        if (packet->tunnel & MOLOCH_PACKET_TUNNEL_MPLS) {
            moloch_session_add_protocol(session, "mpls");
        }

        if (packet->tunnel & MOLOCH_PACKET_TUNNEL_GTP) {
            moloch_session_add_protocol(session, "gtp");
        }

        if (packet->tunnel & MOLOCH_PACKET_TUNNEL_VXLAN) {
            moloch_session_add_protocol(session, "vxlan");
        }
    }

    if (mProtocols[packet->mProtocol].process) {
        // If there is a process callback, call and determine if we free the packet.

        if (mProtocols[packet->mProtocol].process(session, packet))
            moloch_packet_free(packet);

    } else {
        // No process callback, always free

        moloch_packet_free(packet);
    }
}
/******************************************************************************/
#ifndef FUZZLOCH
LOCAL void *moloch_packet_thread(void *threadp)
{
    int thread = (long)threadp;
    const uint32_t maxPackets75 = config.maxPackets*0.75;
    uint32_t skipCount = 0;

    while (1) {
        MolochPacket_t  *packet;

        MOLOCH_LOCK(packetQ[thread].lock);
        inProgress[thread] = 0;
        if (DLL_COUNT(packet_, &packetQ[thread]) == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME_COARSE, &ts);
            currentTime[thread] = ts.tv_sec;
            ts.tv_sec++;
            MOLOCH_COND_TIMEDWAIT(packetQ[thread].lock, ts);

            /* If we are in live capture mode and we haven't received any packets for 10 seconds we set current time to 10
             * seconds in the past so moloch_session_process_commands will clean things up.  10 seconds is arbitrary but
             * we want to make sure we don't set the time ahead of any packets that are currently being read off the wire
             */
            if (!config.pcapReadOffline && DLL_COUNT(packet_, &packetQ[thread]) == 0 && ts.tv_sec - 10 > lastPacketSecs[thread]) {
                lastPacketSecs[thread] = ts.tv_sec - 10;
            }
        }
        inProgress[thread] = 1;
        DLL_POP_HEAD(packet_, &packetQ[thread], packet);
        MOLOCH_UNLOCK(packetQ[thread].lock);

        // Only process commands if the packetQ is less then 75% full or every 8 packets
        if (likely(DLL_COUNT(packet_, &packetQ[thread]) < maxPackets75) || (skipCount & 0x7) == 0) {
            moloch_session_process_commands(thread);
            if (!packet)
                continue;
        } else {
            skipCount++;
        }
        moloch_packet_process(packet, thread);
    }

    return NULL;
}
#endif
/******************************************************************************/
static FILE *unknownPacketFile[3];
LOCAL void moloch_packet_save_unknown_packet(int type, MolochPacket_t * const packet)
{
    static MOLOCH_LOCK_DEFINE(lock);

    struct moloch_pcap_sf_pkthdr hdr;
    hdr.ts.tv_sec  = packet->ts.tv_sec;
    hdr.ts.tv_usec = packet->ts.tv_usec;
    hdr.caplen     = packet->pktlen;
    hdr.pktlen     = packet->pktlen;

    MOLOCH_LOCK(lock);
    if (!unknownPacketFile[type]) {
        char               str[PATH_MAX];
        static const char *names[] = {"unknown.ether", "unknown.ip", "corrupt"};

        snprintf(str, sizeof(str), "%s/%s.%d.pcap", config.pcapDir[0], names[type], getpid());
        unknownPacketFile[type] = fopen(str, "w");

	// TODO-- should we also add logic to pick right pcapDir when there are multiple?
        if (unknownPacketFile[type] == NULL) {
          LOGEXIT("Unable to open pcap file %s to store unknown type %s.  Error %s", str, names[type], strerror (errno));
          MOLOCH_UNLOCK(lock);
          return;
        }

        fwrite(&pcapFileHeader, 24, 1, unknownPacketFile[type]);
    }

    fwrite(&hdr, 16, 1, unknownPacketFile[type]);
    fwrite(packet->pkt, packet->pktlen, 1, unknownPacketFile[type]);
    MOLOCH_UNLOCK(lock);
}

/******************************************************************************/
void moloch_packet_frags_free(MolochFrags_t * const frags)
{
    MolochPacket_t *packet;

    while (DLL_POP_HEAD(packet_, &frags->packets, packet)) {
        moloch_packet_free(packet);
    }
    HASH_REMOVE(fragh_, fragsHash, frags);
    DLL_REMOVE(fragl_, &fragsList, frags);
    MOLOCH_TYPE_FREE(MolochFrags_t, frags);
}
/******************************************************************************/
SUPPRESS_ALIGNMENT
LOCAL gboolean moloch_packet_frags_process(MolochPacket_t * const packet)
{
    MolochPacket_t * fpacket;
    MolochFrags_t   *frags;
    char             key[10];

    struct ip * const ip4 = (struct ip*)(packet->pkt + packet->ipOffset);
    memcpy(key, &ip4->ip_src.s_addr, 4);
    memcpy(key+4, &ip4->ip_dst.s_addr, 4);
    memcpy(key+8, &ip4->ip_id, 2);

    HASH_FIND(fragh_, fragsHash, key, frags);

    if (!frags) {
        frags = MOLOCH_TYPE_ALLOC0(MolochFrags_t);
        memcpy(frags->key, key, 10);
        frags->secs = packet->ts.tv_sec;
        HASH_ADD(fragh_, fragsHash, key, frags);
        DLL_PUSH_TAIL(fragl_, &fragsList, frags);
        DLL_INIT(packet_, &frags->packets);
        DLL_PUSH_TAIL(packet_, &frags->packets, packet);

        if (DLL_COUNT(fragl_, &fragsList) > config.maxFrags) {
            droppedFrags++;
            moloch_packet_frags_free(DLL_PEEK_HEAD(fragl_, &fragsList));
        }
        return FALSE;
    } else {
        DLL_MOVE_TAIL(fragl_, &fragsList, frags);
    }

    uint16_t          ip_off = ntohs(ip4->ip_off);
    uint16_t          ip_flags = ip_off & ~IP_OFFMASK;
    ip_off &= IP_OFFMASK;


    // we might be done once we receive the packets with no flags
    if (ip_flags == 0) {
        frags->haveNoFlags = 1;
    }

    // Insert this packet in correct location sorted by offset
    DLL_FOREACH_REVERSE(packet_, &frags->packets, fpacket) {
        struct ip *fip4 = (struct ip*)(fpacket->pkt + fpacket->ipOffset);
        uint16_t fip_off = ntohs(fip4->ip_off) & IP_OFFMASK;
        if (ip_off >= fip_off) {
            DLL_ADD_AFTER(packet_, &frags->packets, fpacket, packet);
            break;
        }
    }
    if ((void*)fpacket == (void*)&frags->packets) {
        DLL_PUSH_HEAD(packet_, &frags->packets, packet);
    }

    // Don't bother checking until we get a packet with no flags
    if (!frags->haveNoFlags) {
        return FALSE;
    }

    int off = 0;
    struct ip *fip4;

    int payloadLen = 0;
    DLL_FOREACH(packet_, &frags->packets, fpacket) {
        fip4 = (struct ip*)(fpacket->pkt + fpacket->ipOffset);
        uint16_t fip_off = ntohs(fip4->ip_off) & IP_OFFMASK;
        if (fip_off != off)
            break;
        off += fpacket->payloadLen/8;
        payloadLen = MAX(payloadLen, fip_off*8 + fpacket->payloadLen);
    }
    // We have a hole
    if ((void*)fpacket != (void*)&frags->packets) {
        return FALSE;
    }

    // Packet is too large, hacker
    if (payloadLen + packet->payloadOffset >= MOLOCH_PACKET_MAX_LEN) {
        droppedFrags++;
        moloch_packet_frags_free(frags);
        return FALSE;
    }

    // Now alloc the full packet
    packet->pktlen = packet->payloadOffset + payloadLen;
    uint8_t *pkt = malloc(packet->pktlen);

    // Copy packet header
    memcpy(pkt, packet->pkt, packet->payloadOffset);

    // Fix header of new packet
    fip4 = (struct ip*)(pkt + packet->ipOffset);
    fip4->ip_len = htons(payloadLen + 4*ip4->ip_hl);
    fip4->ip_off = 0;

    // Copy payload
    DLL_FOREACH(packet_, &frags->packets, fpacket) {
        fip4 = (struct ip*)(fpacket->pkt + fpacket->ipOffset);
        uint16_t fip_off = ntohs(fip4->ip_off) & IP_OFFMASK;

        if (packet->payloadOffset+(fip_off*8) + fpacket->payloadLen <= packet->pktlen)
            memcpy(pkt+packet->payloadOffset+(fip_off*8), fpacket->pkt+fpacket->payloadOffset, fpacket->payloadLen);
        else
            LOG("WARNING - Not enough room for frag %d > %d", packet->payloadOffset+(fip_off*8) + fpacket->payloadLen, packet->pktlen);
    }

    // Set all the vars in the current packet to new defraged packet
    if (packet->copied)
        free(packet->pkt);
    packet->pkt = pkt;
    packet->copied = 1;
    packet->wasfrag = 1;
    packet->payloadLen = payloadLen;
    DLL_REMOVE(packet_, &frags->packets, packet); // Remove from list so we don't get freed in frags_free
    moloch_packet_frags_free(frags);
    return TRUE;
}
/******************************************************************************/
LOCAL void moloch_packet_frags4(MolochPacketBatch_t *batch, MolochPacket_t * const packet)
{
    MolochFrags_t *frags;

    // ALW - Should change frags_process to make the copy when needed
    if (!packet->copied) {
        uint8_t *pkt = malloc(packet->pktlen);
        memcpy(pkt, packet->pkt, packet->pktlen);
        packet->pkt = pkt;
        packet->copied = 1;
    }

    MOLOCH_LOCK(frags);
    // Remove expired entries
    while ((frags = DLL_PEEK_HEAD(fragl_, &fragsList)) && (frags->secs + config.fragsTimeout < packet->ts.tv_sec)) {
        droppedFrags++;
        moloch_packet_frags_free(frags);
    }

    gboolean process = moloch_packet_frags_process(packet);
    MOLOCH_UNLOCK(frags);

    if (process)
        moloch_packet_batch(batch, packet);
}
/******************************************************************************/
int moloch_packet_frags_size()
{
    return DLL_COUNT(fragl_, &fragsList);
}
/******************************************************************************/
int moloch_packet_frags_outstanding()
{
    return 0;
}
/******************************************************************************/
LOCAL void moloch_packet_log(int ses)
{
    MolochReaderStats_t stats;
    if (moloch_reader_stats(&stats)) {
        stats.dropped = 0;
        stats.total = totalPackets;
    }

    LOG("packets: %" PRIu64 " current sessions: %u/%u oldest: %d - recv: %" PRIu64 " drop: %" PRIu64 " (%0.2f) queue: %d disk: %d packet: %d close: %d ns: %d frags: %d/%d pstats: %" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64,
      totalPackets,
      moloch_session_watch_count(ses),
      moloch_session_monitoring(),
      moloch_session_idle_seconds(ses),
      stats.total,
      stats.dropped - initialDropped,
      (stats.total?(stats.dropped - initialDropped)*(double)100.0/stats.total:0),
      moloch_http_queue_length(esServer),
      moloch_writer_queue_length(),
      moloch_packet_outstanding(),
      moloch_session_close_outstanding(),
      moloch_session_need_save_outstanding(),
      moloch_packet_frags_outstanding(),
      moloch_packet_frags_size(),
      packetStats[MOLOCH_PACKET_DO_PROCESS],
      packetStats[MOLOCH_PACKET_IP_DROPPED],
      packetStats[MOLOCH_PACKET_OVERLOAD_DROPPED],
      packetStats[MOLOCH_PACKET_CORRUPT],
      packetStats[MOLOCH_PACKET_UNKNOWN],
      packetStats[MOLOCH_PACKET_IPPORT_DROPPED]
      );

      if (config.debug > 0) {
          moloch_rules_stats();
      }
}
/******************************************************************************/
LOCAL int moloch_packet_ip_gtp(MolochPacketBatch_t *batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 12) {
        return MOLOCH_PACKET_CORRUPT;
    }
    BSB bsb;
    BSB_INIT(bsb, data, len);

    uint8_t  flags = 0;
    uint8_t  next = 0;


    BSB_IMPORT_u08(bsb, flags);
    BSB_IMPORT_skip(bsb, 1); // mtype
    BSB_IMPORT_skip(bsb, 2); // mlen
    BSB_IMPORT_skip(bsb, 4); // teid
    if (flags & 0x7) {
        BSB_IMPORT_skip(bsb, 3);
        BSB_IMPORT_u08(bsb, next);
    }

    while (next != 0 && !BSB_IS_ERROR(bsb)) {
        uint8_t extlen = 0;
        BSB_IMPORT_u08(bsb, extlen);
        if (extlen == 0) {
            return MOLOCH_PACKET_CORRUPT;
        }
        BSB_IMPORT_skip(bsb, extlen*4-2);
        BSB_IMPORT_u08(bsb, next);
    }

    if (BSB_IS_ERROR(bsb)) {
        return MOLOCH_PACKET_CORRUPT;
    }

    packet->tunnel |= MOLOCH_PACKET_TUNNEL_GTP;

    // Should check for v4 vs v6 here
    BSB_IMPORT_u08(bsb, flags);
    BSB_IMPORT_rewind(bsb, 1);

    if ((flags & 0xf0) == 0x60)
        return moloch_packet_ip6(batch, packet, BSB_WORK_PTR(bsb), BSB_REMAINING(bsb));
    return moloch_packet_ip4(batch, packet, BSB_WORK_PTR(bsb), BSB_REMAINING(bsb));
}
/******************************************************************************/
LOCAL int moloch_packet_ip4_vxlan(MolochPacketBatch_t *batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 8) {
        return MOLOCH_PACKET_CORRUPT;
    }

    packet->tunnel |= MOLOCH_PACKET_TUNNEL_VXLAN;

    return moloch_packet_ether(batch, packet, data+8, len-8);
}
/******************************************************************************/
SUPPRESS_ALIGNMENT
LOCAL int moloch_packet_ip4(MolochPacketBatch_t *batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    struct ip           *ip4 = (struct ip*)data;
    struct tcphdr       *tcphdr = 0;
    struct udphdr       *udphdr = 0;
    char                 sessionId[MOLOCH_SESSIONID_LEN];

#ifdef DEBUG_PACKET
    LOG("enter %p %p %d", packet, data, len);
#endif

    if (len < (int)sizeof(struct ip)) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: too small for header %p %d", packet, len);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }

    if (ip4->ip_v != 4) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: ip4->ip_v4 %d != 4", ip4->ip_v);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }

    int ip_len = ntohs(ip4->ip_len);
    if (len < ip_len) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: incomplete %p %d %d", packet, len, ip_len);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }

    int ip_hdr_len = 4 * ip4->ip_hl;
    if (ip_hdr_len < 4 * 5 || len < ip_hdr_len || ip_len < ip_hdr_len) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: too small for header and options %p %d %d", packet, len, ip_hdr_len);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }
    if (ipTree4) {
        patricia_node_t *node;

        if ((node = patricia_search_best3 (ipTree4, (u_char*)&ip4->ip_src, 32)) && node->data == NULL)
            return MOLOCH_PACKET_IP_DROPPED;

        if ((node = patricia_search_best3 (ipTree4, (u_char*)&ip4->ip_dst, 32)) && node->data == NULL)
            return MOLOCH_PACKET_IP_DROPPED;
    }

    if ((uint8_t*)data - packet->pkt >= 2048)
        return MOLOCH_PACKET_CORRUPT;

    packet->ipOffset = (uint8_t*)data - packet->pkt;
    packet->v6 = 0;
    packet->payloadOffset = packet->ipOffset + ip_hdr_len;
    packet->payloadLen = ip_len - ip_hdr_len;

    uint16_t ip_off = ntohs(ip4->ip_off);
    uint16_t ip_flags = ip_off & ~IP_OFFMASK;
    ip_off &= IP_OFFMASK;


    if ((ip_flags & IP_MF) || ip_off > 0) {
        moloch_packet_frags4(batch, packet);
        return MOLOCH_PACKET_DONT_PROCESS_OR_FREE;
    }

    packet->ipProtocol = ip4->ip_p;
    switch (ip4->ip_p) {
    case IPPROTO_IPV4:
        return moloch_packet_ip4(batch, packet, data + ip_hdr_len, len - ip_hdr_len);
        break;
    case IPPROTO_TCP:
        if (len < ip_hdr_len + (int)sizeof(struct tcphdr)) {
#ifdef DEBUG_PACKET
            LOG("BAD PACKET: too small for tcp hdr %p %d", packet, len);
#endif
            return MOLOCH_PACKET_CORRUPT;
        }

        tcphdr = (struct tcphdr *)((char*)ip4 + ip_hdr_len);

        if (packetDrop4.drops[tcphdr->th_sport] &&
            moloch_drophash_should_drop(&packetDrop4, tcphdr->th_sport, &ip4->ip_src.s_addr, packet->ts.tv_sec)) {

            return MOLOCH_PACKET_IPPORT_DROPPED;
        }

        if (packetDrop4.drops[tcphdr->th_dport] &&
            moloch_drophash_should_drop(&packetDrop4, tcphdr->th_dport, &ip4->ip_dst.s_addr, packet->ts.tv_sec)) {

            return MOLOCH_PACKET_IPPORT_DROPPED;
        }

        moloch_session_id(sessionId, ip4->ip_src.s_addr, tcphdr->th_sport,
                          ip4->ip_dst.s_addr, tcphdr->th_dport);
        packet->mProtocol = tcpMProtocol;

        break;
    case IPPROTO_UDP:
        if (len < ip_hdr_len + (int)sizeof(struct udphdr)) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: too small for udp header %p %d", packet, len);
#endif
            return MOLOCH_PACKET_CORRUPT;
        }

        udphdr = (struct udphdr *)((char*)ip4 + ip_hdr_len);

        // See if this is really GTP
        if (udphdr->uh_dport == 0x6808 && len > ip_hdr_len + (int)sizeof(struct udphdr) + 12) {
            int rem = len - ip_hdr_len - sizeof(struct udphdr *);
            uint8_t *buf = (uint8_t *)ip4 + ip_hdr_len + sizeof(struct udphdr *);
            if ((buf[0] & 0xf0) == 0x30 && buf[1] == 0xff && (buf[2] << 8 | buf[3]) == rem - 8) {
                return moloch_packet_ip_gtp(batch, packet, buf, rem);
            }
        }

        // See if this is really VXLAN
        if (udphdr->uh_dport == 0xb512 && len > ip_hdr_len + (int)sizeof(struct udphdr) + 8) {
            int rem = len - ip_hdr_len - sizeof(struct udphdr *);
            uint8_t *buf = (uint8_t *)ip4 + ip_hdr_len + sizeof(struct udphdr *);
            if ((buf[0] & 0x77) == 0 && (buf[1] & 0xb7) == 0) {
                return moloch_packet_ip4_vxlan(batch, packet, buf, rem);
            }
        }

        moloch_session_id(sessionId, ip4->ip_src.s_addr, udphdr->uh_sport,
                          ip4->ip_dst.s_addr, udphdr->uh_dport);
        packet->mProtocol = udpMProtocol;
        break;
    case IPPROTO_IPV6:
        return moloch_packet_ip6(batch, packet, data + ip_hdr_len, len - ip_hdr_len);
    default:
        return moloch_packet_run_ip_cb(batch, packet, data + ip_hdr_len, len - ip_hdr_len, ip4->ip_p, "IP4");
    }
    packet->hash = moloch_session_hash(sessionId);
    return MOLOCH_PACKET_DO_PROCESS;
}
/******************************************************************************/
SUPPRESS_ALIGNMENT
LOCAL int moloch_packet_ip6(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    struct ip6_hdr      *ip6 = (struct ip6_hdr *)data;
    struct tcphdr       *tcphdr = 0;
    struct udphdr       *udphdr = 0;
    char                 sessionId[MOLOCH_SESSIONID_LEN];

#ifdef DEBUG_PACKET
    LOG("enter %p %p %d", packet, data, len);
#endif

    if (len < (int)sizeof(struct ip6_hdr)) {
        return MOLOCH_PACKET_CORRUPT;
    }

    int ip_len = ntohs(ip6->ip6_plen);
    if (len < ip_len) {
        return MOLOCH_PACKET_CORRUPT;
    }

    if (ipTree6) {
        patricia_node_t *node;

        if ((node = patricia_search_best3 (ipTree6, (u_char*)&ip6->ip6_src, 128)) && node->data == NULL)
            return MOLOCH_PACKET_IP_DROPPED;

        if ((node = patricia_search_best3 (ipTree6, (u_char*)&ip6->ip6_dst, 128)) && node->data == NULL)
            return MOLOCH_PACKET_IP_DROPPED;
    }

    int ip_hdr_len = sizeof(struct ip6_hdr);

    packet->ipOffset = (uint8_t*)data - packet->pkt;
    packet->v6 = 1;

    packet->payloadOffset = packet->ipOffset + ip_hdr_len;

    if (ip_len + (int)sizeof(struct ip6_hdr) < ip_hdr_len) {
#ifdef DEBUG_PACKET
        LOG ("ERROR - %d + %ld < %d", ip_len, (long)sizeof(struct ip6_hdr), ip_hdr_len);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }
    packet->payloadLen = ip_len + sizeof(struct ip6_hdr) - ip_hdr_len;

    if (packet->pktlen < packet->payloadOffset + packet->payloadLen) {
#ifdef DEBUG_PACKET
        LOG ("ERROR - %d < %d + %d", packet->pktlen, packet->payloadOffset, packet->payloadLen);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }


#ifdef DEBUG_PACKET
    LOG("Got ip6 header %p %d", packet, packet->pktlen);
#endif
    int nxt = ip6->ip6_nxt;
    int done = 0;
    do {
        packet->ipProtocol = nxt;

        switch (nxt) {
        case IPPROTO_HOPOPTS:
        case IPPROTO_DSTOPTS:
        case IPPROTO_ROUTING:
            if (len < ip_hdr_len + 2) {
#ifdef DEBUG_PACKET
                LOG("ERROR - %d < %d + 2", len, ip_hdr_len);
#endif
                return MOLOCH_PACKET_CORRUPT;
            }
            nxt = data[ip_hdr_len];
            ip_hdr_len += ((data[ip_hdr_len+1] + 1) << 3);

            packet->payloadOffset = packet->ipOffset + ip_hdr_len;

            if (ip_len + (int)sizeof(struct ip6_hdr) < ip_hdr_len) {
#ifdef DEBUG_PACKET
                LOG ("ERROR - %d + %ld < %d", ip_len, (long)sizeof(struct ip6_hdr), ip_hdr_len);
#endif
                return MOLOCH_PACKET_CORRUPT;
            }
            packet->payloadLen = ip_len + sizeof(struct ip6_hdr) - ip_hdr_len;

            if (packet->pktlen < packet->payloadOffset + packet->payloadLen) {
#ifdef DEBUG_PACKET
                LOG ("ERROR - %d < %d + %d", packet->pktlen, packet->payloadOffset, packet->payloadLen);
#endif
                return MOLOCH_PACKET_CORRUPT;
            }

            break;
        case IPPROTO_FRAGMENT:
#ifdef DEBUG_PACKET
            LOG("ERROR - Don't support ip6 fragements yet!");
#endif
            return MOLOCH_PACKET_UNKNOWN;

        case IPPROTO_TCP:
            if (len < ip_hdr_len + (int)sizeof(struct tcphdr)) {
                return MOLOCH_PACKET_CORRUPT;
            }

            tcphdr = (struct tcphdr *)(data + ip_hdr_len);


            if (packetDrop6.drops[tcphdr->th_sport] &&
                moloch_drophash_should_drop(&packetDrop6, tcphdr->th_sport, &ip6->ip6_src, packet->ts.tv_sec)) {

                return MOLOCH_PACKET_IPPORT_DROPPED;
            }

            if (packetDrop6.drops[tcphdr->th_dport] &&
                moloch_drophash_should_drop(&packetDrop6, tcphdr->th_dport, &ip6->ip6_dst, packet->ts.tv_sec)) {

                return MOLOCH_PACKET_IPPORT_DROPPED;
            }

            moloch_session_id6(sessionId, ip6->ip6_src.s6_addr, tcphdr->th_sport,
                               ip6->ip6_dst.s6_addr, tcphdr->th_dport);
            packet->mProtocol = tcpMProtocol;
            done = 1;
            break;
        case IPPROTO_UDP:
            if (len < ip_hdr_len + (int)sizeof(struct udphdr)) {
                return MOLOCH_PACKET_CORRUPT;
            }

            udphdr = (struct udphdr *)(data + ip_hdr_len);

            moloch_session_id6(sessionId, ip6->ip6_src.s6_addr, udphdr->uh_sport,
                               ip6->ip6_dst.s6_addr, udphdr->uh_dport);

            // See if this is really GTP
            if (udphdr->uh_dport == 0x6808 && len > ip_hdr_len + (int)sizeof(struct udphdr) + 12) {
                int rem = len - ip_hdr_len - sizeof(struct udphdr *);
                const uint8_t *buf = (uint8_t *)udphdr + sizeof(struct udphdr *);
                if ((buf[0] & 0xf0) == 0x30 && buf[1] == 0xff && (buf[2] << 8 | buf[3]) == rem - 8) {
                    return moloch_packet_ip_gtp(batch, packet, buf, rem);
                }
            }

            packet->mProtocol = udpMProtocol;
            done = 1;
            break;
        case IPPROTO_IPV4:
            return moloch_packet_ip4(batch, packet, data + ip_hdr_len, len - ip_hdr_len);
        case IPPROTO_IPV6:
            return moloch_packet_ip6(batch, packet, data + ip_hdr_len, len - ip_hdr_len);
        default:
            return moloch_packet_run_ip_cb(batch, packet, data + ip_hdr_len, len - ip_hdr_len, nxt, "IP6");
        }
        if (ip_hdr_len > len) {
#ifdef DEBUG_PACKET
            LOG ("ERROR - Corrupt packet ip_hdr_len = %d nxt = %d len = %d", ip_hdr_len, nxt, len);
#endif
            return MOLOCH_PACKET_CORRUPT;
        }
    } while (!done);

    packet->hash = moloch_session_hash(sessionId);
    return MOLOCH_PACKET_DO_PROCESS;
}
/******************************************************************************/
LOCAL int moloch_packet_frame_relay(MolochPacketBatch_t *batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 4)
        return MOLOCH_PACKET_CORRUPT;

    uint16_t type = data[2] << 8 | data[3];

    if (type == 0x03cc)
        return moloch_packet_ip4(batch, packet, data+4, len-4);

    return moloch_packet_run_ethernet_cb(batch, packet, data+4, len-4, type, "FrameRelay");
}
/******************************************************************************/
LOCAL int moloch_packet_ieee802(MolochPacketBatch_t *batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
#ifdef DEBUG_PACKET
    LOG("enter %p %p %d", packet, data, len);
#endif

    if (len < 6 || memcmp(data+2, "\xfe\xfe\x03", 3) != 0)
        return MOLOCH_PACKET_CORRUPT;

    int etherlen = data[0] << 8 | data[+1];
    int ethertype = data[5];

    if (etherlen > len - 2)
        return MOLOCH_PACKET_CORRUPT;

    return moloch_packet_run_ethernet_cb(batch, packet, data+6, len-6, ethertype, "ieee802");
}
/******************************************************************************/
LOCAL int moloch_packet_ether(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 14) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: Too short %d", len);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }
    int n = 12;
    while (n+2 < len) {
        int ethertype = data[n] << 8 | data[n+1];
        if (ethertype <= 1500) {
            return moloch_packet_ieee802(batch, packet, data+n, len-n);
        }
        n += 2;
        switch (ethertype) {
        case 0x8100:
            n += 2;
            break;
        default:
            return moloch_packet_run_ethernet_cb(batch, packet, data+n,len-n, ethertype, "Ether");
        } // switch
    }
#ifdef DEBUG_PACKET
    LOG("BAD PACKET: bad len %d < %d", n+2, len);
#endif
    return MOLOCH_PACKET_CORRUPT;
}
/******************************************************************************/
LOCAL int moloch_packet_sll(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 16) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: Too short %d", len);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }

    int ethertype = data[14] << 8 | data[15];
    switch (ethertype) {
    case 0x8100:
        if ((data[20] & 0xf0) == 0x60)
            return moloch_packet_ip6(batch, packet, data+20, len - 20);
        else
            return moloch_packet_ip4(batch, packet, data+20, len - 20);
    default:
        return moloch_packet_run_ethernet_cb(batch, packet, data+16,len-16, ethertype, "SLL");
    } // switch
    return MOLOCH_PACKET_CORRUPT;
}
/******************************************************************************/
LOCAL int moloch_packet_nflog(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 14 ||
        (data[0] != AF_INET && data[0] != AF_INET6) ||
        data[1] != 0) {
#ifdef DEBUG_PACKET
        LOG("BAD PACKET: Wrong type %d", data[0]);
#endif
        return MOLOCH_PACKET_CORRUPT;
    }
    int n = 4;
    while (n+4 < len) {
        int length = data[n+1] << 8 | data[n];

        // Make sure length is at least header and not bigger then remaining packet
        if (length < 4 || length > len - n) {
#ifdef DEBUG_PACKET
            LOG("BAD PACKET: Wrong len %d", length);
#endif
            return MOLOCH_PACKET_CORRUPT;
        }

        if (data[n+3] == 0 && data[n+2] == 9) {
            if (data[0] == AF_INET) {
                return moloch_packet_ip4(batch, packet, data+n+4, length - 4);
            } else {
                return moloch_packet_ip6(batch, packet, data+n+4, length - 4);
            }
        } else {
            n += ((length + 3) & 0xfffffc);
        }
    }
#ifdef DEBUG_PACKET
    LOG("BAD PACKET: Not sure");
#endif
    return MOLOCH_PACKET_CORRUPT;
}
/******************************************************************************/
LOCAL int moloch_packet_radiotap(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (data[0] != 0 || len < 36)
        return MOLOCH_PACKET_UNKNOWN;

    int hl = packet->pkt[2];
    if (hl + 24 + 8 >= len)
        return MOLOCH_PACKET_UNKNOWN;

    if (data[hl] != 8)
        return MOLOCH_PACKET_UNKNOWN;

    hl += 24 + 3;

    if (data[hl] != 0 || data[hl+1] != 0 || data[hl+2] != 0)
        return MOLOCH_PACKET_UNKNOWN;

    hl += 3;

    uint16_t ethertype = (data[hl] << 8) | data[hl+1];
    hl += 2;

    return moloch_packet_run_ethernet_cb(batch, packet, data+hl,len-hl, ethertype, "RadioTap");
}
/******************************************************************************/
void moloch_packet_batch_init(MolochPacketBatch_t *batch)
{
    int t;

    for (t = 0; t < config.packetThreads; t++) {
        DLL_INIT(packet_, &batch->packetQ[t]);
    }
    batch->count = 0;
}
/******************************************************************************/
void moloch_packet_batch_flush(MolochPacketBatch_t *batch)
{
    int t;

    for (t = 0; t < config.packetThreads; t++) {
        if (DLL_COUNT(packet_, &batch->packetQ[t]) > 0) {
            MOLOCH_LOCK(packetQ[t].lock);
            DLL_PUSH_TAIL_DLL(packet_, &packetQ[t], &batch->packetQ[t]);
            MOLOCH_COND_SIGNAL(packetQ[t].lock);
            MOLOCH_UNLOCK(packetQ[t].lock);
        }
    }
    batch->count = 0;
}
/******************************************************************************/
void moloch_packet_batch(MolochPacketBatch_t * batch, MolochPacket_t * const packet)
{
    int rc;

#ifdef DEBUG_PACKET
    LOG("enter %p %u %d", packet, pcapFileHeader.linktype, packet->pktlen);
    moloch_print_hex_string(packet->pkt, packet->pktlen);
#endif

    switch(pcapFileHeader.linktype) {
    case 0: // NULL
        if (packet->pktlen > 4) {
            if (packet->pkt[0] == 30)
                rc = moloch_packet_ip6(batch, packet, packet->pkt+4, packet->pktlen-4);
            else
                rc = moloch_packet_ip4(batch, packet, packet->pkt+4, packet->pktlen-4);
        } else {
#ifdef DEBUG_PACKET
            LOG("BAD PACKET: Too short %d", packet->pktlen);
#endif
            rc = MOLOCH_PACKET_CORRUPT;
        }
        break;
    case 1: // Ether
        rc = moloch_packet_ether(batch, packet, packet->pkt, packet->pktlen);
        break;
    case 12: // LOOP
    case 101: // RAW
        rc = moloch_packet_ip4(batch, packet, packet->pkt, packet->pktlen);
        break;
    case 107: // Frame Relay
        rc = moloch_packet_frame_relay(batch, packet, packet->pkt, packet->pktlen);
        break;
    case 113: // SLL
        if (packet->pkt[0] == 0 && packet->pkt[1] <= 4)
            rc = moloch_packet_sll(batch, packet, packet->pkt, packet->pktlen);
        else
            rc = moloch_packet_ip4(batch, packet, packet->pkt, packet->pktlen);
        break;
    case 127: // radiotap
        rc = moloch_packet_radiotap(batch, packet, packet->pkt, packet->pktlen);
        break;
    case 228: //RAW IPv4
        rc = moloch_packet_ip4(batch, packet, packet->pkt, packet->pktlen);
        break;
    case 239: // NFLOG
        rc = moloch_packet_nflog(batch, packet, packet->pkt, packet->pktlen);
        break;
    default:
        if (config.ignoreErrors)
            rc = MOLOCH_PACKET_CORRUPT;
        else
            LOGEXIT("ERROR - Unsupported pcap link type %u", pcapFileHeader.linktype);
    }
    if (unlikely(packet->mProtocol == 0) && likely(rc == MOLOCH_PACKET_DO_PROCESS)) {
        if (config.debug)
            LOG("Packet was market as do process but no mProtocol was set");
        rc = MOLOCH_PACKET_UNKNOWN;
    }

    if (rc == MOLOCH_PACKET_CORRUPT) {
      if (ipCbs[MOLOCH_IPPROTO_CORRUPT]) {
        ipCbs[MOLOCH_IPPROTO_CORRUPT](batch, packet, packet->pkt, packet->pktlen);
      }
    }


    MOLOCH_THREAD_INCR(packetStats[rc]);

    if (rc) {
      if (unlikely(rc == MOLOCH_PACKET_CORRUPT) && config.corruptSavePcap) {
          moloch_packet_save_unknown_packet(2, packet);
      }
      if (! ((rc == MOLOCH_PACKET_CORRUPT) && (ipCbs[MOLOCH_IPPROTO_CORRUPT]))) {
        if (rc != MOLOCH_PACKET_DONT_PROCESS_OR_FREE)
            moloch_packet_free(packet);
        return;
      }
    }

    /* This packet we are going to process */

    if (unlikely(totalPackets == 0)) {
        MolochReaderStats_t stats;
        if (!moloch_reader_stats(&stats)) {
            initialDropped = stats.dropped;
        }
        initialPacket = packet->ts;
        if (!config.pcapReadOffline)
            LOG("Initial Packet = %ld Initial Dropped = %u", initialPacket.tv_sec, initialDropped);
    }

    MOLOCH_THREAD_INCR(totalPackets);
    if (totalPackets % config.logEveryXPackets == 0) {
        moloch_packet_log(mProtocols[packet->mProtocol].ses);
    }

    uint32_t thread = packet->hash % config.packetThreads;

    totalBytes[thread] += packet->pktlen;

    if (DLL_COUNT(packet_, &packetQ[thread]) >= config.maxPacketsInQueue) {
        MOLOCH_LOCK(packetQ[thread].lock);
        overloadDrops[thread]++;
        if ((overloadDrops[thread] % 10000) == 1) {
            LOG("WARNING - Packet Q %u is overflowing, total dropped so far %u.  See https://molo.ch/faq#why-am-i-dropping-packets and modify %s", thread, overloadDrops[thread], config.configFile);
        }
        packet->pkt = 0;
        MOLOCH_COND_SIGNAL(packetQ[thread].lock);
        MOLOCH_UNLOCK(packetQ[thread].lock);
        MOLOCH_THREAD_INCR(packetStats[rc]);
        moloch_packet_free(packet);
        return;
    }

    if (!packet->copied) {
        uint8_t *pkt = malloc(packet->pktlen);
        memcpy(pkt, packet->pkt, packet->pktlen);
        packet->pkt = pkt;
        packet->copied = 1;
    }

#ifdef FUZZLOCH
    moloch_session_process_commands(thread);
    moloch_packet_process(packet, thread);
#else
    DLL_PUSH_TAIL(packet_, &batch->packetQ[thread], packet);
#endif
    batch->count++;
}
/******************************************************************************/
int moloch_packet_outstanding()
{
    int count = 0;
    int t;

    for (t = 0; t < config.packetThreads; t++) {
        count += DLL_COUNT(packet_, &packetQ[t]);
        count += inProgress[t];
    }
    return count;
}
/******************************************************************************/
SUPPRESS_UNSIGNED_INTEGER_OVERFLOW
SUPPRESS_SIGNED_INTEGER_OVERFLOW
LOCAL uint32_t moloch_packet_frag_hash(const void *key)
{
    int i;
    uint32_t n = 0;
    for (i = 0; i < 10; i++) {
        n = (n << 5) - n + ((char*)key)[i];
    }
    return n;
}
/******************************************************************************/
LOCAL int moloch_packet_frag_cmp(const void *keyv, const MolochFrags_t *element)
{
    return memcmp(keyv, element->key, 10) == 0;
}
/******************************************************************************/
LOCAL gboolean moloch_packet_save_drophash(gpointer UNUSED(user_data))
{
    if (packetDrop4.changed)
        moloch_drophash_save(&packetDrop4);

    if (packetDrop6.changed)
        moloch_drophash_save(&packetDrop6);

    return TRUE;
}
/******************************************************************************/
void moloch_packet_save_ethernet( MolochPacket_t * const packet, uint16_t type)
{
    if (BIT_ISSET(type, config.etherSavePcap))
        moloch_packet_save_unknown_packet(0, packet);
}
/******************************************************************************/
int moloch_packet_run_ethernet_cb(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len, uint16_t type, const char *str)
{
#ifdef DEBUG_PACKET
    LOG("enter %p %d %s %p %d", packet, type, str, data, len);
#endif

    if (ethernetCbs[type]) {
        return ethernetCbs[type](batch, packet, data, len);
    }

    if (ethernetCbs[MOLOCH_ETHERTYPE_UNKNOWN]) {
      return ethernetCbs[MOLOCH_ETHERTYPE_UNKNOWN](batch, packet, data, len);
    }

    if (config.logUnknownProtocols)
        LOG("Unknown %s ethernet protocol 0x%04x(%d)", str, type, type);
    moloch_packet_save_ethernet(packet, type);
    return MOLOCH_PACKET_UNKNOWN;
}
/******************************************************************************/
void moloch_packet_set_ethernet_cb(uint16_t type, MolochPacketEnqueue_cb enqueueCb)
{
    if (ethernetCbs[type]) 
      LOG ("redining existing callback type %d", type);

    ethernetCbs[type] = enqueueCb;
}
/******************************************************************************/
int moloch_packet_run_ip_cb(MolochPacketBatch_t * batch, MolochPacket_t * const packet, const uint8_t *data, int len, uint16_t type, const char *str)
{
#ifdef DEBUG_PACKET
    LOG("enter %p %d %s %p %d", packet, type, str, data, len);
#endif

    if (type >= 0x110) {
        return MOLOCH_PACKET_CORRUPT;
    }

    if (ipCbs[type]) {
        return ipCbs[type](batch, packet, data, len);
    }

    if (ipCbs[MOLOCH_IPPROTO_UNKNOWN]) {
        return ipCbs[MOLOCH_IPPROTO_UNKNOWN](batch, packet, data, len);
    }

    if (config.logUnknownProtocols)
        LOG("Unknown %s protocol %d", str, type);
    if (BIT_ISSET(type, config.ipSavePcap))
        moloch_packet_save_unknown_packet(1, packet);
    return MOLOCH_PACKET_UNKNOWN;
}
/******************************************************************************/
void moloch_packet_set_ip_cb(uint16_t type, MolochPacketEnqueue_cb enqueueCb)
{
    if (type > 0x110) 
      LOGEXIT ("type value to large %d", type);

    ipCbs[type] = enqueueCb;
}
/******************************************************************************/
void moloch_packet_init()
{
    pcapFileHeader.magic = 0xa1b2c3d4;
    pcapFileHeader.version_major = 2;
    pcapFileHeader.version_minor = 4;

    pcapFileHeader.thiszone = 0;
    pcapFileHeader.sigfigs = 0;

    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "/tmp/%s.tcp.drops.4", config.nodeName);
    moloch_drophash_init(&packetDrop4, filename, TRUE);

    snprintf(filename, sizeof(filename), "/tmp/%s.tcp.drops.6", config.nodeName);
    moloch_drophash_init(&packetDrop6, filename, FALSE);
    g_timeout_add_seconds(10, moloch_packet_save_drophash, 0);

    mac1Field = moloch_field_define("general", "lotermfield",
        "mac.src", "Src MAC", "srcMac",
        "Source ethernet mac addresses set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        "transform", "dash2Colon",
        (char *)NULL);

    mac2Field = moloch_field_define("general", "lotermfield",
        "mac.dst", "Dst MAC", "dstMac",
        "Destination ethernet mac addresses set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        "transform", "dash2Colon",
        (char *)NULL);

    moloch_field_define("general", "lotermfield",
        "mac", "Src or Dst MAC", "macall",
        "Shorthand for mac.src or mac.dst",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        "regex", "^mac\\\\.(?:(?!\\\\.cnt$).)*$",
        "transform", "dash2Colon",
        (char *)NULL);

    oui1Field = moloch_field_define("general", "termfield",
        "oui.src", "Src OUI", "srcOui",
        "Source ethernet oui set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        (char *)NULL);

    oui2Field = moloch_field_define("general", "termfield",
        "oui.dst", "Dst OUI", "dstOui",
        "Destination ethernet oui set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        (char *)NULL);


    vlanField = moloch_field_define("general", "integer",
        "vlan", "VLan", "vlan",
        "vlan value",
        MOLOCH_FIELD_TYPE_INT_GHASH,  MOLOCH_FIELD_FLAG_CNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        (char *)NULL);

    greIpField = moloch_field_define("general", "ip",
        "gre.ip", "GRE IP", "greIp",
        "GRE ip addresses for session",
        MOLOCH_FIELD_TYPE_IP_GHASH,  MOLOCH_FIELD_FLAG_CNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.syn", "TCP Flag SYN", "tcpflags.syn",
        "Count of packets with SYN and no ACK flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.syn-ack", "TCP Flag SYN-ACK", "tcpflags.syn-ack",
        "Count of packets with SYN and ACK flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.ack", "TCP Flag ACK", "tcpflags.ack",
        "Count of packets with only the ACK flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.psh", "TCP Flag PSH", "tcpflags.psh",
        "Count of packets with PSH flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.fin", "TCP Flag FIN", "tcpflags.fin",
        "Count of packets with FIN flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.rst", "TCP Flag RST", "tcpflags.rst",
        "Count of packets with RST flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "tcpflags.urg", "TCP Flag URG", "tcpflags.urg",
        "Count of packets with URG flag set",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "packets.src", "Src Packets", "srcPackets",
        "Total number of packets sent by source in a session",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "packets.dst", "Dst Packets", "dstPackets",
        "Total number of packets sent by destination in a session",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "integer",
        "initRTT", "Initial RTT", "initRTT",
        "Initial round trip time, difference between SYN and ACK timestamp divided by 2 in ms",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    moloch_field_define("general", "termfield",
        "communityId", "Community Id", "communityId",
        "Community id flow hash",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        (char *)NULL);

    int t;
    for (t = 0; t < config.packetThreads; t++) {
        char name[100];
        DLL_INIT(packet_, &packetQ[t]);
        MOLOCH_LOCK_INIT(packetQ[t].lock);
        MOLOCH_COND_INIT(packetQ[t].lock);
        snprintf(name, sizeof(name), "moloch-pkt%d", t);
#ifndef FUZZLOCH
        g_thread_unref(g_thread_new(name, &moloch_packet_thread, (gpointer)(long)t));
#endif
    }

    HASH_INIT(fragh_, fragsHash, moloch_packet_frag_hash, (HASH_CMP_FUNC)moloch_packet_frag_cmp);
    DLL_INIT(fragl_, &fragsList);

    moloch_add_can_quit(moloch_packet_outstanding, "packet outstanding");
    moloch_add_can_quit(moloch_packet_frags_outstanding, "packet frags outstanding");


    moloch_packet_set_ethernet_cb(MOLOCH_ETHERTYPE_ETHER, moloch_packet_ether);
    moloch_packet_set_ethernet_cb(0x6559, moloch_packet_frame_relay);
    moloch_packet_set_ethernet_cb(ETHERTYPE_IP, moloch_packet_ip4);
    moloch_packet_set_ethernet_cb(ETHERTYPE_IPV6, moloch_packet_ip6);
}
/******************************************************************************/
uint64_t moloch_packet_dropped_packets()
{
    MolochReaderStats_t stats;
    if (moloch_reader_stats(&stats)) {
        return 0;
    }
    return stats.dropped - initialDropped;
}
/******************************************************************************/
uint64_t moloch_packet_dropped_frags()
{
    return droppedFrags;
}
/******************************************************************************/
uint64_t moloch_packet_dropped_overload()
{
    uint64_t count = 0;

    int t;

    for (t = 0; t < config.packetThreads; t++) {
        count += overloadDrops[t];
    }
    return count;
}
/******************************************************************************/
uint64_t moloch_packet_total_bytes()
{
    uint64_t count = 0;

    int t;

    for (t = 0; t < config.packetThreads; t++) {
        count += totalBytes[t];
    }
    return count;
}
/******************************************************************************/
void moloch_packet_add_packet_ip(char *ipstr, int mode)
{
    patricia_node_t *node;
    if (strchr(ipstr, '.') != 0) {
        if (!ipTree4)
            ipTree4 = New_Patricia(32);
        node = make_and_lookup(ipTree4, ipstr);
    } else {
        if (!ipTree6)
            ipTree6 = New_Patricia(128);
        node = make_and_lookup(ipTree6, ipstr);
    }
    node->data = (void *)(long)mode;
}
/******************************************************************************/
void moloch_packet_set_linksnap(int linktype, int snaplen)
{
    pcapFileHeader.linktype = linktype;
    pcapFileHeader.snaplen = snaplen;
    moloch_rules_recompile();
}
/******************************************************************************/
void moloch_packet_drophash_add(MolochSession_t *session, int which, int min)
{
    if (session->ses != SESSION_TCP)
        return;

    // packetDrop is kept in network byte order
    const int port = (which == 0)?htons(session->port1):htons(session->port2);

    if (MOLOCH_SESSION_v6(session)) {
        if (which == 0) {
            moloch_drophash_add(&packetDrop6, port, (void*)&session->addr1, session->lastPacket.tv_sec, min*60);
        } else {
            moloch_drophash_add(&packetDrop6, port, (void*)&session->addr2, session->lastPacket.tv_sec, min*60);
        }
    } else {
        if (which == 0) {
            moloch_drophash_add(&packetDrop4, port, &((uint32_t *)session->addr1.s6_addr)[3], session->lastPacket.tv_sec, min*60);
        } else {
            moloch_drophash_add(&packetDrop4, port, &((uint32_t *)session->addr2.s6_addr)[3], session->lastPacket.tv_sec, min*60);
        }
    }
}
/******************************************************************************/
void moloch_packet_exit()
{
    if (ipTree4) {
        Destroy_Patricia(ipTree4, NULL);
        ipTree4 = 0;
    }

    if (ipTree6) {
        Destroy_Patricia(ipTree6, NULL);
        ipTree6 = 0;
    }
    moloch_packet_log(SESSION_TCP);
    if (unknownPacketFile[0])
        fclose(unknownPacketFile[0]);
    if (unknownPacketFile[1])
        fclose(unknownPacketFile[1]);
    if (unknownPacketFile[2])
        fclose(unknownPacketFile[2]);
}
/******************************************************************************/
int moloch_mprotocol_register_internal(char                            *name,
                                       int                              ses,
                                       MolochProtocolCreateSessionId_cb createSessionId,
                                       MolochProtocolPreProcess_cb      preProcess,
                                       MolochProtocolProcess_cb         process,
                                       MolochProtocolSessionFree_cb     sFree,
                                       size_t                           sessionsize,
                                       int                              apiversion)
{
    if (sizeof(MolochSession_t) != sessionsize) {
        LOGEXIT("Parser '%s' built with different version of moloch.h\n %u != %u", name, (unsigned int)sizeof(MolochSession_t),  (unsigned int)sessionsize);
    }

    if (MOLOCH_API_VERSION != apiversion) {
        LOGEXIT("Parser '%s' built with different version of moloch.h\n %d %d", name, MOLOCH_API_VERSION, apiversion);
    }

    int num = ++mProtocolCnt; // Leave 0 empty so we know if not set in code
    mProtocols[num].name = name;
    mProtocols[num].ses = ses;
    mProtocols[num].createSessionId = createSessionId;
    mProtocols[num].preProcess = preProcess;
    mProtocols[num].process = process;
    mProtocols[num].sFree = sFree;
    return num;
}
