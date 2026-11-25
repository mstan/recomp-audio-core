#include "recomp_audio.h"
#include "oracle_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static uint32_t rng=42;
static void fill(int16_t *s,int n) {
    for(int i=0;i<n;++i){rng=rng*1664525u+1013904223u;s[i]=(int16_t)(rng>>16);}
}
static void compare_state(const rab_bridge *b,int profile,void *o) {
    oracle_state s;oracle_observe(profile,o,&s);
    CHECK(b->in_count==s.in_count);CHECK(b->out_pos==s.out_pos);CHECK(b->cur_step==s.step);
    CHECK(b->err_lp==s.err);CHECK(b->corr==s.corr);CHECK(b->gain==s.gain);
    CHECK(b->prime_ms==s.prime);CHECK(b->primed==s.primed);CHECK(b->concealing==s.concealing);
    CHECK(b->loop_start==s.loop_start);CHECK(b->loop_len==s.loop_len);CHECK(b->loop_pos==s.loop_pos);
    CHECK(memcmp(b->last_out,s.last_out,sizeof(s.last_out))==0);
    CHECK(b->stats.pushed_frames==s.pushed);CHECK(b->stats.pulled_frames==s.pulled);
    CHECK(b->stats.underrun_events==s.underruns);CHECK(b->stats.overflow_drops==s.overflow);
    CHECK(b->stats.stretch_frames==s.stretch);CHECK(b->stats.stretch_events==s.episodes);
    CHECK(b->stats.last_fill_ms==s.fill);CHECK(b->stats.last_correction==s.correction);
    uint64_t hash=UINT64_C(14695981039346656037);
    const unsigned char *p=(const unsigned char *)b->ring;
    for(size_t i=0;i<(size_t)b->cap*b->cfg.channels*sizeof(float);++i)
        hash=(hash^p[i])*UINT64_C(1099511628211);
    CHECK(hash==s.ring_hash);
}
static void pair_pull(rab_bridge *b,int profile,void *o,int n) {
    int16_t actual[4096],expected[4096];CHECK(n*b->cfg.channels<=4096);
    rab_pull(b,actual,n);oracle_pull(profile,o,expected,n);
    CHECK(memcmp(actual,expected,(size_t)n*b->cfg.channels*sizeof(int16_t))==0);
    compare_state(b,profile,o);
}
static void scenario(int profile,double source,double host,int channels,int stretch) {
    rab_config c;rab_config_defaults(&c);c.delivery_profile=profile;c.source_rate=source;
    c.host_rate=host;c.channels=channels;c.stretch_enable=stretch;
    rab_bridge b;CHECK(rab_init(&b,&c)==0);
    void *o=oracle_create(profile,source,host,channels,stretch);CHECK(o);
    int max=(int)b.cap*2+103;int16_t *samples=(int16_t *)malloc((size_t)max*channels*sizeof(int16_t));CHECK(samples);
    fill(samples,max*channels);compare_state(&b,profile,o);
    pair_pull(&b,profile,o,173); /* cold, unprimed output */
    int prime=(int)(source*0.065);rab_push(&b,samples,prime);oracle_push(profile,o,samples,prime);
    pair_pull(&b,profile,o,257);
    for(int i=0;i<130;++i) {
        int n=(int)(source/host*97)+(i%3-1)*3;
        rab_push(&b,samples,n);oracle_push(profile,o,samples,n);pair_pull(&b,profile,o,97);
    }
    for(int i=0;i<30;++i)pair_pull(&b,profile,o,257); /* starvation and loop-wrap */
    int small=(int)(source*0.012);rab_push(&b,samples,small);oracle_push(profile,o,samples,small);
    pair_pull(&b,profile,o,311); /* incomplete recovery differs by profile */
    rab_push(&b,samples,prime);oracle_push(profile,o,samples,prime);pair_pull(&b,profile,o,257);
    rab_push(&b,samples,max);oracle_push(profile,o,samples,max);pair_pull(&b,profile,o,257);
    CHECK(b.stats.overflow_drops>0);CHECK(b.stats.pulled_frames>0);
    rab_reset(&b);oracle_reset(profile,o);compare_state(&b,profile,o);pair_pull(&b,profile,o,173);
    free(samples);oracle_destroy(profile,o);rab_free(&b);
}
static void prepared(void) {
    rab_config c;rab_config_defaults(&c);rab_bridge a,b;
    CHECK(!rab_init(&a,&c));CHECK(!rab_init(&b,&c));
    int16_t s[2048];float f[2048];fill(s,2048);
    for(int i=0;i<2048;++i)f[i]=(float)s[i]*(1.0f/32768.0f);
    for(int i=0;i<40;++i){
        int16_t x[512],y[512];rab_push(&a,s,1024);
        if(!rab_try_push_prepared(&b,f,1024))rab_push(&b,s,1024);
        CHECK(a.in_count==b.in_count);CHECK(a.out_pos==b.out_pos);
        CHECK(memcmp(a.ring,b.ring,(size_t)a.cap*2*sizeof(float))==0);
        rab_pull(&a,x,256);rab_pull(&b,y,256);CHECK(!memcmp(x,y,sizeof(x)));
    }
    b.in_count=UINT64_C(9007199254740992);double pos=b.out_pos;
    CHECK(!rab_try_push_prepared(&b,f,1024));CHECK(b.in_count==UINT64_C(9007199254740992));CHECK(b.out_pos==pos);
    rab_free(&a);rab_free(&b);
}
static void interface_checks(void) {
    rab_config c;rab_config_defaults(&c);recomp_audio *a=recomp_audio_create(&c);CHECK(a);
    int16_t s[400];fill(s,400);recomp_audio_push(a,s,200);recomp_audio_pull(a,s,200);
    rab_stats stats;recomp_audio_stats(a,&stats);CHECK(stats.pushed_frames==200&&stats.pulled_frames==200);
    recomp_audio_reset(a);recomp_audio_stats(a,&stats);CHECK(!stats.pushed_frames&&!stats.pulled_frames);
    recomp_audio_destroy(a);recomp_audio_destroy(NULL);
    c.source_rate=NAN;CHECK(!recomp_audio_create(&c));rab_config_defaults(&c);
    c.target_ms=0;CHECK(!recomp_audio_create(&c));rab_config_defaults(&c);
    c.source_rate=INFINITY;CHECK(!recomp_audio_create(&c));rab_config_defaults(&c);
    c.ring_ms=1e300;CHECK(!recomp_audio_create(&c));rab_config_defaults(&c);
    c.channels=3;CHECK(!recomp_audio_create(&c));rab_config_defaults(&c);
    c.delivery_profile=12;CHECK(!recomp_audio_create(&c));
}
int main(void) {
    const double rates[]={22050,32768,44100,48000,223721};
    int cases=0;
    for(int p=0;p<2;++p)for(int r=0;r<5;++r)for(int h=0;h<2;++h)
        for(int c=1;c<=2;++c)for(int t=0;t<2;++t){scenario(p,rates[r],h?44100:48000,c,t);++cases;}
    prepared();interface_checks();printf("%d frozen-donor differential scenarios, prepared-push and lifecycle checks passed\n",cases);return 0;
}
