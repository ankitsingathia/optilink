#include "optilink/eth.h"

#include <stddef.h>
#include <string.h>

/* Nibble wide CRC table. The usual byte wide table is 1 KB, which is a real
 * fraction of this part's flash; sixteen entries cost 64 bytes and still run
 * about four times faster than the bit at a time loop. At the frame rates
 * the management path sees, that is the right end of the trade. */
static const uint32_t CRC32_NIBBLE[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

static eth_stats_t s_stats;

uint32_t eth_crc32(const uint8_t *buf, uint16_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint16_t i = 0u; i < len; i++) {
        crc ^= buf[i];
        crc = (crc >> 4) ^ CRC32_NIBBLE[crc & 0x0Fu];
        crc = (crc >> 4) ^ CRC32_NIBBLE[crc & 0x0Fu];
    }
    return ~crc;
}

void eth_reset_stats(void)
{
    memset(&s_stats, 0, sizeof s_stats);
}

const eth_stats_t *eth_stats(void)
{
    return &s_stats;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t le32(const uint8_t *p)
{
    /* The FCS goes onto the wire least significant byte first, unlike every
     * other multi-byte field in the frame. */
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

eth_result_t eth_parse(const uint8_t *buf, uint16_t len, eth_frame_t *out)
{
    eth_frame_t f;
    uint16_t    off;
    uint32_t    computed;

    memset(&f, 0, sizeof f);

    if (buf == NULL) {
        s_stats.truncated++;
        return ETH_ERR_TRUNCATED;
    }

    s_stats.frames++;
    s_stats.bytes += len;

    if (len < ETH_MIN_FRAME) {
        s_stats.undersize++;
        return ETH_ERR_UNDERSIZE;
    }
    if (len > ETH_MAX_FRAME) {
        s_stats.oversize++;
        return ETH_ERR_OVERSIZE;
    }

    memcpy(f.dst, &buf[0], ETH_ADDR_LEN);
    memcpy(f.src, &buf[ETH_ADDR_LEN], ETH_ADDR_LEN);
    off = ETH_ADDR_LEN * 2u;

    /* Walk the tag stack. Outer first, and at most two: anything deeper is
     * not something this data path is specified to carry. */
    f.ethertype = be16(&buf[off]);
    off += 2u;

    if (f.ethertype == ETH_TPID_8021AD || f.ethertype == ETH_TPID_8021Q) {
        uint16_t tci;

        if ((uint16_t)(off + 2u) > len) {
            s_stats.truncated++;
            return ETH_ERR_TRUNCATED;
        }
        tci           = be16(&buf[off]);
        f.vlan_tagged = true;
        f.outer_pcp   = (uint8_t)((tci >> 13) & 0x07u);
        f.outer_dei   = ((tci >> 12) & 0x01u) != 0u;
        f.outer_vid   = (uint16_t)(tci & 0x0FFFu);
        off += 2u;

        if ((uint16_t)(off + 2u) > len) {
            s_stats.truncated++;
            return ETH_ERR_TRUNCATED;
        }
        f.ethertype = be16(&buf[off]);
        off += 2u;

        if (f.ethertype == ETH_TPID_8021Q) {
            if ((uint16_t)(off + 4u) > len) {
                s_stats.truncated++;
                return ETH_ERR_TRUNCATED;
            }
            tci         = be16(&buf[off]);
            f.qinq      = true;
            f.inner_pcp = (uint8_t)((tci >> 13) & 0x07u);
            f.inner_vid = (uint16_t)(tci & 0x0FFFu);
            off += 2u;
            f.ethertype = be16(&buf[off]);
            off += 2u;
        }
    }

    if ((uint16_t)(off + ETH_FCS_LEN) > len) {
        s_stats.truncated++;
        return ETH_ERR_TRUNCATED;
    }

    f.payload     = &buf[off];
    f.payload_len = (uint16_t)(len - off - ETH_FCS_LEN);

    /* Address classification is a property of the destination's first octet:
     * bit 0 marks a group address, and all ones is the broadcast case. */
    if (f.dst[0] == 0xFFu && f.dst[1] == 0xFFu && f.dst[2] == 0xFFu &&
        f.dst[3] == 0xFFu && f.dst[4] == 0xFFu && f.dst[5] == 0xFFu) {
        s_stats.broadcast++;
    } else if ((f.dst[0] & 0x01u) != 0u) {
        s_stats.multicast++;
    } else {
        s_stats.unicast++;
    }

    if (f.qinq) {
        s_stats.qinq++;
    }
    if (f.vlan_tagged) {
        s_stats.vlan_tagged++;
    }

    computed        = eth_crc32(buf, (uint16_t)(len - ETH_FCS_LEN));
    f.fcs_computed  = computed;
    f.fcs_in_frame  = le32(&buf[len - ETH_FCS_LEN]);

    if (out != NULL) {
        *out = f;
    }

    if (f.fcs_in_frame != computed) {
        s_stats.fcs_errors++;
        return ETH_ERR_BAD_FCS;
    }
    return ETH_OK;
}

const char *eth_result_name(eth_result_t r)
{
    switch (r) {
    case ETH_OK:            return "OK";
    case ETH_ERR_UNDERSIZE: return "UNDERSIZE";
    case ETH_ERR_OVERSIZE:  return "OVERSIZE";
    case ETH_ERR_TRUNCATED: return "TRUNCATED";
    case ETH_ERR_BAD_FCS:   return "BAD_FCS";
    default:                return "?";
    }
}
