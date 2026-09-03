#include "optilink/eth.h"

#include <string.h>

#include "suites.h"
#include "tinytest.h"

static uint8_t s_buf[ETH_MAX_FRAME + 8u];

/* Appends a correct FCS after len bytes and returns the new total length.
 * The FCS goes on the wire least significant byte first. */
static uint16_t seal(uint16_t len)
{
    uint32_t crc = eth_crc32(s_buf, len);

    s_buf[len + 0u] = (uint8_t)(crc & 0xFFu);
    s_buf[len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    s_buf[len + 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    s_buf[len + 3u] = (uint8_t)((crc >> 24) & 0xFFu);
    return (uint16_t)(len + 4u);
}

static void set_mac(uint8_t *p, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                    uint8_t e, uint8_t f)
{
    p[0] = a;
    p[1] = b;
    p[2] = c;
    p[3] = d;
    p[4] = e;
    p[5] = f;
}

static void be16_at(uint16_t off, uint16_t v)
{
    s_buf[off]      = (uint8_t)(v >> 8);
    s_buf[off + 1u] = (uint8_t)(v & 0xFFu);
}

/* Builds an untagged frame with a payload long enough to reach the 64 byte
 * minimum, and returns its total length. */
static uint16_t build_basic(void)
{
    memset(s_buf, 0, sizeof s_buf);
    set_mac(&s_buf[0], 0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u);
    set_mac(&s_buf[6], 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu);
    be16_at(12u, ETH_TYPE_IPV4);
    for (uint16_t i = 0u; i < 46u; i++) {
        s_buf[14u + i] = (uint8_t)i;
    }
    return seal(60u);
}

void suite_eth(void)
{
    eth_frame_t f;
    uint16_t    len;

    TT_CASE("crc32 matches the standard check value");
    /* The reference vector for reflected CRC-32: "123456789" is 0xCBF43926.
     * If this is right, the FCS logic is right. */
    CHECK_EQ(eth_crc32((const uint8_t *)"123456789", 9u), 0xCBF43926u);
    CHECK_EQ(eth_crc32((const uint8_t *)"", 0u), 0u);

    TT_CASE("a well formed frame parses");
    eth_reset_stats();
    len = build_basic();
    CHECK_EQ(len, 64);
    CHECK_EQ(eth_parse(s_buf, len, &f), ETH_OK);
    CHECK_EQ(f.dst[0], 0x00);
    CHECK_EQ(f.dst[5], 0x55);
    CHECK_EQ(f.src[0], 0xAA);
    CHECK_EQ(f.ethertype, ETH_TYPE_IPV4);
    CHECK(!f.vlan_tagged);
    CHECK(!f.qinq);
    CHECK_EQ(f.payload_len, 46);
    CHECK_EQ(f.fcs_in_frame, f.fcs_computed);
    CHECK_EQ(eth_stats()->frames, 1);
    CHECK_EQ(eth_stats()->unicast, 1);
    CHECK_EQ(eth_stats()->fcs_errors, 0);

    TT_CASE("a single corrupted bit is caught");
    eth_reset_stats();
    len = build_basic();
    s_buf[20] ^= 0x01u;
    CHECK_EQ(eth_parse(s_buf, len, &f), ETH_ERR_BAD_FCS);
    CHECK(f.fcs_in_frame != f.fcs_computed);
    CHECK_EQ(eth_stats()->fcs_errors, 1);
    /* A bad FCS is still a frame that arrived, and still counts as one. */
    CHECK_EQ(eth_stats()->frames, 1);

    TT_CASE("802.1Q tag is decoded");
    eth_reset_stats();
    memset(s_buf, 0, sizeof s_buf);
    set_mac(&s_buf[0], 0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u);
    set_mac(&s_buf[6], 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu);
    be16_at(12u, ETH_TPID_8021Q);
    /* PCP 5, DEI 0, VID 100 -> 0xA064 */
    be16_at(14u, 0xA064u);
    be16_at(16u, ETH_TYPE_IPV6);
    len = seal(64u);
    CHECK_EQ(eth_parse(s_buf, len, &f), ETH_OK);
    CHECK(f.vlan_tagged);
    CHECK(!f.qinq);
    CHECK_EQ(f.outer_vid, 100);
    CHECK_EQ(f.outer_pcp, 5);
    CHECK_EQ(f.outer_dei, 0);
    /* The reported ethertype is the inner one, not the TPID. */
    CHECK_EQ(f.ethertype, ETH_TYPE_IPV6);
    CHECK_EQ(eth_stats()->vlan_tagged, 1);

    TT_CASE("802.1ad double tag is decoded");
    eth_reset_stats();
    memset(s_buf, 0, sizeof s_buf);
    set_mac(&s_buf[0], 0x01u, 0x80u, 0xC2u, 0x00u, 0x00u, 0x0Eu); /* multicast */
    set_mac(&s_buf[6], 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu);
    be16_at(12u, ETH_TPID_8021AD);
    be16_at(14u, 0x2FA0u); /* PCP 1, VID 4000 */
    be16_at(16u, ETH_TPID_8021Q);
    be16_at(18u, 0x400Au); /* PCP 2, VID 10 */
    be16_at(20u, ETH_TYPE_ARP);
    len = seal(68u);
    CHECK_EQ(eth_parse(s_buf, len, &f), ETH_OK);
    CHECK(f.vlan_tagged);
    CHECK(f.qinq);
    CHECK_EQ(f.outer_vid, 4000);
    CHECK_EQ(f.outer_pcp, 1);
    CHECK_EQ(f.inner_vid, 10);
    CHECK_EQ(f.inner_pcp, 2);
    CHECK_EQ(f.ethertype, ETH_TYPE_ARP);
    CHECK_EQ(eth_stats()->qinq, 1);
    CHECK_EQ(eth_stats()->multicast, 1);
    CHECK_EQ(eth_stats()->broadcast, 0);

    TT_CASE("broadcast is not counted as multicast");
    eth_reset_stats();
    len = build_basic();
    memset(&s_buf[0], 0xFF, 6u);
    len = seal(60u);
    CHECK_EQ(eth_parse(s_buf, len, &f), ETH_OK);
    CHECK_EQ(eth_stats()->broadcast, 1);
    CHECK_EQ(eth_stats()->multicast, 0);
    CHECK_EQ(eth_stats()->unicast, 0);

    TT_CASE("runt and giant are rejected before parsing");
    eth_reset_stats();
    len = build_basic();
    CHECK_EQ(eth_parse(s_buf, 63u, &f), ETH_ERR_UNDERSIZE);
    CHECK_EQ(eth_stats()->undersize, 1);
    CHECK_EQ(eth_parse(s_buf, ETH_MAX_FRAME + 1u, &f), ETH_ERR_OVERSIZE);
    CHECK_EQ(eth_stats()->oversize, 1);
    /* Exactly at the limits is legal on both ends. */
    CHECK(eth_parse(s_buf, 64u, &f) != ETH_ERR_UNDERSIZE);

    TT_CASE("a null buffer is refused rather than dereferenced");
    eth_reset_stats();
    CHECK_EQ(eth_parse(NULL, 64u, &f), ETH_ERR_TRUNCATED);
    CHECK_EQ(eth_stats()->truncated, 1);
    CHECK_EQ(eth_stats()->frames, 0);

    TT_CASE("out may be null");
    eth_reset_stats();
    len = build_basic();
    CHECK_EQ(eth_parse(s_buf, len, NULL), ETH_OK);
    CHECK_EQ(eth_stats()->frames, 1);

    TT_CASE("result names are all populated");
    CHECK_STR(eth_result_name(ETH_OK), "OK");
    CHECK_STR(eth_result_name(ETH_ERR_BAD_FCS), "BAD_FCS");
    CHECK_STR(eth_result_name((eth_result_t)99), "?");
}
