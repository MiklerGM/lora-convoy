/* Host ABI for the actual node/codec and an explicitly experimental forwarding adapter. */
#include "node.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_HEADER 4
#define MAX_FORWARD 8

typedef struct {
    uint32_t version, slot, slots, interval_ms, duty_permille, listen_one_in;
    int32_t nmea_lag_us, nmea_err_us;
    uint32_t seed, sf, bandwidth_hz, coding_rate, preamble, frequency_hz, sync_word;
    uint32_t experimental_relay, relay_records;
    int64_t relay_ttl_us;
    uint32_t implicit_header, crc;
    uint8_t key[16];
} sim_config_t;

typedef struct {
    uint32_t slot, session, counter, fix_time, status;
    int32_t lat_e7, lon_e7, speed_kmh, course2, rssi_dbm, snr_db4;
    int64_t fix_utc_ms, heard_us;
} sim_record_t;

typedef struct {
    sim_config_t config;
    node_t node;
    uint8_t cache[NODE_SLOTS][PKT_POSITION_LEN];
    bool cached[NODE_SLOTS];
    bool mode_pending;
    int64_t cached_at[NODE_SLOTS], forwarded_at[NODE_SLOTS];
} sim_node_t;

uint32_t sim_abi_version(void) { return 1; }
size_t sim_config_size(void) { return sizeof(sim_config_t); }
size_t sim_record_size(void) { return sizeof(sim_record_t); }

static phy_t mode(const sim_config_t *c) {
    return (phy_t){.freq_hz=c->frequency_hz, .bw_hz=c->bandwidth_hz, .sf=(uint8_t)c->sf,
                   .cr=(uint8_t)c->coding_rate, .preamble=(uint16_t)c->preamble,
                   .sync={(uint8_t)(c->sync_word >> 8), (uint8_t)c->sync_word}};
}

static uint32_t airtime(const sim_node_t *s, size_t length) {
    const phy_t *p = &s->node.phy;
    const int sf=p->sf, de=phy_ldro(p), divisor=4*(sf-2*de);
    const int bits=8*(int)length-4*sf+28+16*(int)s->config.crc-20*(int)s->config.implicit_header;
    const unsigned blocks=bits>0 ? (unsigned)((bits+divisor-1)/divisor) : 0;
    const unsigned symbols4=4*p->preamble+17+4*(8+blocks*p->cr);
    return (uint32_t)((uint64_t)symbols4*phy_symbol_us(p)/4);
}

void *sim_node_create(const sim_config_t *c) {
    if (!c || c->version!=1 || c->slots<1 || c->slots>NODE_SLOTS || c->slot>=c->slots ||
        c->interval_ms<100 || c->interval_ms>60000 || c->duty_permille>1000 ||
        c->listen_one_in>UINT16_MAX || c->relay_records>MAX_FORWARD || c->relay_ttl_us<=0 ||
        c->implicit_header>1 || c->crc>1 || c->experimental_relay>1 || !c->seed ||
        c->nmea_lag_us<0 || c->nmea_err_us<0 || c->sf<7 || c->sf>12 ||
        c->coding_rate<5 || c->coding_rate>8 || c->preamble<6 || c->preamble>UINT16_MAX ||
        c->sync_word>UINT16_MAX ||
        (!c->experimental_relay && (c->implicit_header || !c->crc))) return NULL;
    phy_t p=mode(c);
    if (!phy_supported(&p)) return NULL;
    sim_node_t *s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->config=*c;
    node_config_t cfg={.keyed=true, .slot=(uint8_t)c->slot, .slots=(uint8_t)c->slots,
        .interval_ms=c->interval_ms, .duty_permille=(uint16_t)c->duty_permille, .phy=p,
        .nmea_lag_us=c->nmea_lag_us, .nmea_err_us=c->nmea_err_us,
        .listen_one_in=(uint16_t)c->listen_one_in};
    memcpy(cfg.key,c->key,sizeof(cfg.key));
    node_init(&s->node,&cfg,c->seed);
    return s;
}

void sim_node_destroy(void *p) { free(p); }
void sim_node_pps(void *p,int64_t at) { node_on_pps(&((sim_node_t*)p)->node,at); }
void sim_node_help(void *p,int on) { node_set_help(&((sim_node_t*)p)->node,on!=0); }

void sim_node_fix(void *p,int64_t at,int64_t utc_ms,int valid,int time_valid,
                  int32_t lat,int32_t lon,int32_t speed,int32_t course) {
    gnss_fix_t f={.time_valid=time_valid!=0,.valid=valid!=0,.utc_ms=utc_ms,.lat_e7=lat,.lon_e7=lon,
                  .speed_kmh10=speed,.course_deg10=course,.sats=9,.hdop10=9};
    node_on_fix(&((sim_node_t*)p)->node,&f,at);
}

int sim_node_clock(void *p,int64_t at,int64_t *utc,int64_t *err) {
    return node_utc_us(&((sim_node_t*)p)->node,at,utc,err);
}
int64_t sim_node_next(void *p,int64_t now) { return node_next_tx_us(&((sim_node_t*)p)->node,now); }
void sim_node_tx_done(void *p,int64_t at) { node_on_tx_done(&((sim_node_t*)p)->node,at); }

void sim_node_mode(void *p,const sim_config_t *c,int applied) {
    sim_node_t *s=p;
    phy_t phy=mode(c);
    if (!phy_supported(&phy)) return;
    if (applied) {
        s->config.implicit_header=c->implicit_header;
        s->config.crc=c->crc;
        node_phy_applied(&s->node,&phy);
        s->mode_pending=false;
    } else {
        s->node.cfg.phy=phy;
        s->mode_pending=true;
    }
}

static void export_record(const position_t *p,const rider_t *r,sim_record_t *out) {
    *out=(sim_record_t){.slot=p->slot,.session=p->session,.counter=p->counter,.fix_time=p->fix_time,
        .status=p->status,.lat_e7=p->lat_e7,.lon_e7=p->lon_e7,.speed_kmh=p->speed_kmh,
        .course2=p->course2,.rssi_dbm=r?r->rssi_dbm:0,.snr_db4=r?r->snr_db4:0,
        .fix_utc_ms=r?r->fix_utc_ms:0,.heard_us=r?r->heard_us:0};
}

int sim_node_view(void *p,unsigned slot,sim_record_t *out) {
    sim_node_t *s=p;
    if (slot>=NODE_SLOTS || !out || !s->node.riders[slot].known) return 0;
    const rider_t *r=&s->node.riders[slot];
    export_record(&r->last,r,out);
    return 1;
}

static int frame_count(const uint8_t *b,size_t len) {
    if (len==PKT_POSITION_LEN) return 1;
    if (len<FRAME_HEADER || b[0]!='S' || b[1]!='R' || b[2]!=1 || b[3]<1 || b[3]>MAX_FORWARD+1 ||
        len!=FRAME_HEADER+(size_t)b[3]*PKT_POSITION_LEN) return 0;
    return b[3];
}

int sim_frame_record(void *p,const uint8_t *b,size_t len,unsigned index,sim_record_t *out) {
    if (!p || !b || !out || index>=(unsigned)frame_count(b,len)) return 0;
    const uint8_t *record=b+(len==PKT_POSITION_LEN ? 0 : FRAME_HEADER+index*PKT_POSITION_LEN);
    position_t decoded;
    if (pkt_open(record,PKT_POSITION_LEN,&((sim_node_t*)p)->node.group,&decoded)!=PKT_OK) return 0;
    export_record(&decoded,NULL,out);
    return 1;
}

uint32_t sim_node_receive(void *p,const uint8_t *b,size_t len,int rssi,int snr,int64_t at,int64_t stamp) {
    sim_node_t *s=p;
    const int count=frame_count(b,len);
    if (!count || (!s->config.experimental_relay && len!=PKT_POSITION_LEN)) return 0;
    if (!s->config.experimental_relay) {
        if (!node_on_packet(&s->node,b,len,rssi,snr,at)) return 0;
        position_t decoded;
        if (pkt_open(b,len,&s->node.group,&decoded)!=PKT_OK) return 0;
        if (stamp>=0) node_on_rx_done(&s->node,stamp);
        return 1u<<decoded.slot;
    }
    uint32_t accepted=0;
    for (int i=0;i<count;i++) {
        const uint8_t *record=b+(len==PKT_POSITION_LEN ? 0 : FRAME_HEADER+(size_t)i*PKT_POSITION_LEN);
        position_t decoded;
        if (pkt_open(record,PKT_POSITION_LEN,&s->node.group,&decoded)!=PKT_OK || decoded.slot>=s->node.cfg.slots)
            continue;
        if (s->config.experimental_relay && decoded.slot==s->node.cfg.slot) continue;
        if (node_on_packet(&s->node,record,PKT_POSITION_LEN,rssi,snr,at)) {
            accepted |= 1u<<decoded.slot;
            memcpy(s->cache[decoded.slot],record,PKT_POSITION_LEN);
            s->cached[decoded.slot]=true;
            s->cached_at[decoded.slot]=at;
            if (len==PKT_POSITION_LEN && stamp>=0) node_on_rx_done(&s->node,stamp);
        }
    }
    return accepted;
}

size_t sim_node_poll(void *p,int64_t at,uint8_t *out,size_t capacity) {
    sim_node_t *s=p;
    if (s->mode_pending) {
        int64_t utc,err;
        if (node_next_tx_us(&s->node,at)<=at && node_utc_us(&s->node,at,&utc,&err)) {
            s->node.last_frame=utc/((int64_t)s->node.cfg.interval_ms*1000);
            s->node.tx_held++;
        }
        return 0;
    }
    if (!s->config.experimental_relay) return node_poll_tx(&s->node,at,out,capacity);
    unsigned chosen[MAX_FORWARD], count=0;
    for (unsigned k=0;k<s->config.relay_records;k++) {
        int best=-1;
        for (unsigned slot=0;slot<s->node.cfg.slots;slot++) {
            if (slot==s->node.cfg.slot || !s->cached[slot] || at-s->cached_at[slot]>s->config.relay_ttl_us) continue;
            bool already=false;
            for (unsigned j=0;j<count;j++) already |= chosen[j]==slot;
            if (!already && (best<0 || s->forwarded_at[slot]<s->forwarded_at[best])) best=(int)slot;
        }
        if (best<0) break;
        chosen[count++]=(unsigned)best;
    }
    int64_t utc,err;
    if (!node_utc_us(&s->node,at,&utc,&err)) return 0;
    size_t length;
    uint32_t air_us;
    for (;;) {
        length=FRAME_HEADER+(1+count)*PKT_POSITION_LEN;
        air_us=airtime(s,length);
        const int64_t spacing=(int64_t)s->node.cfg.interval_ms*1000/(s->node.cfg.slots+1);
        const uint32_t maximum_air=airtime(s,FRAME_HEADER+(1+s->config.relay_records)*PKT_POSITION_LEN);
        const int64_t budget=(spacing-maximum_air-2000)/2;
        const bool fits=length<=capacity && budget>=err+1000 &&
            node_duty_used_ms(&s->node,at)+(air_us+999)/1000<=3600u*s->node.cfg.duty_permille;
        if (fits) break;
        if (!count) {
            /* Consume an unworkable slot through the real core without booking an RF transmission. */
            uint8_t ignored[PKT_POSITION_LEN];
            node_poll_tx(&s->node,at,ignored,0);
            return 0;
        }
        count--;
    }
    uint8_t own[PKT_POSITION_LEN];
    if (!node_poll_tx(&s->node,at,own,sizeof(own))) return 0;
    const int bucket=(int)((at/60000000)%60);
    const uint32_t base_ms=(phy_airtime_us(&s->node.phy,PKT_POSITION_LEN)+999)/1000;
    s->node.duty.used_ms[bucket]-=base_ms;
    s->node.duty.used_ms[bucket]+=(air_us+999)/1000;
    s->node.tx_air_us=air_us;
    out[0]='S';out[1]='R';out[2]=1;out[3]=(uint8_t)(1+count);
    memcpy(out+FRAME_HEADER,own,sizeof(own));
    for (unsigned j=0;j<count;j++) {
        memcpy(out+FRAME_HEADER+(j+1)*PKT_POSITION_LEN,s->cache[chosen[j]],PKT_POSITION_LEN);
        s->forwarded_at[chosen[j]]=at;
    }
    return length;
}

uint32_t sim_node_duty(void *p,int64_t now) { return node_duty_used_ms(&((sim_node_t*)p)->node,now); }
uint32_t sim_node_unsure(void *p) { return ((sim_node_t*)p)->node.tx_unsure; }
int64_t sim_node_target_utc(void *p) {
    const node_t *n=&((sim_node_t*)p)->node;
    const int64_t frame=(int64_t)n->cfg.interval_ms*1000;
    return n->last_frame*frame+n->phase_us+n->cfg.slot*(frame/(n->cfg.slots+1));
}
