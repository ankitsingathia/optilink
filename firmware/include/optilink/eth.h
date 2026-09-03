/*
 * eth.h - layer 2 frame inspection.
 *
 * The module controller is not a switch and does not forward anything. What
 * it does is look at frames arriving on the management data path so that a
 * link which is optically healthy but electrically broken can be told apart
 * from one that is simply idle. A lane can sit at DP_ACTIVATED with good
 * optical power and still deliver nothing but FCS errors, and only a frame
 * level counter will show it.
 *
 * Handles untagged, 802.1Q single tagged and 802.1ad double tagged frames,
 * and validates the FCS.
 */
#ifndef OPTILINK_ETH_H
#define OPTILINK_ETH_H

#include <stdbool.h>
#include <stdint.h>

#define ETH_ADDR_LEN     6u
#define ETH_HDR_LEN      14u
#define ETH_FCS_LEN      4u
#define ETH_MIN_FRAME    64u   /* including FCS, per 802.3 */
#define ETH_MAX_FRAME    1526u /* 1518 plus two VLAN tags */

#define ETH_TPID_8021Q   0x8100u
#define ETH_TPID_8021AD  0x88A8u
#define ETH_TYPE_IPV4    0x0800u
#define ETH_TYPE_ARP     0x0806u
#define ETH_TYPE_IPV6    0x86DDu

typedef enum {
    ETH_OK = 0,
    ETH_ERR_UNDERSIZE,
    ETH_ERR_OVERSIZE,
    ETH_ERR_TRUNCATED,
    ETH_ERR_BAD_FCS
} eth_result_t;

typedef struct {
    uint8_t  dst[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t ethertype;   /* the first type that is not a TPID */

    bool     vlan_tagged;
    bool     qinq;
    uint16_t outer_vid;   /* 12 bits */
    uint8_t  outer_pcp;   /* 3 bits */
    bool     outer_dei;
    uint16_t inner_vid;
    uint8_t  inner_pcp;

    const uint8_t *payload;
    uint16_t       payload_len;

    uint32_t fcs_in_frame;
    uint32_t fcs_computed;
} eth_frame_t;

typedef struct {
    uint32_t frames;
    uint32_t bytes;
    uint32_t fcs_errors;
    uint32_t undersize;
    uint32_t oversize;
    uint32_t truncated;
    uint32_t vlan_tagged;
    uint32_t qinq;
    uint32_t unicast;
    uint32_t multicast;
    uint32_t broadcast;
} eth_stats_t;

void               eth_reset_stats(void);
const eth_stats_t *eth_stats(void);

/* Parses one frame and folds it into the counters. out may be NULL when the
 * caller only wants the statistics. */
eth_result_t eth_parse(const uint8_t *buf, uint16_t len, eth_frame_t *out);

/* Ethernet FCS: reflected CRC-32, polynomial 0xEDB88320. */
uint32_t eth_crc32(const uint8_t *buf, uint16_t len);

const char *eth_result_name(eth_result_t r);

#endif /* OPTILINK_ETH_H */
