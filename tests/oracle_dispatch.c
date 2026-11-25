#include "oracle_api.h"
#define DECL(P) \
void *fixture_##P##_create(double,double,int,int); \
void fixture_##P##_destroy(void *); \
void fixture_##P##_push(void *,const int16_t *,int); \
void fixture_##P##_pull(void *,int16_t *,int); \
void fixture_##P##_observe(void *,oracle_state *); \
void fixture_##P##_reset(void *);
DECL(genesis)
DECL(nes)
void *oracle_create(int p,double s,double h,int c,int t) {
    return p?fixture_nes_create(s,h,c,t):fixture_genesis_create(s,h,c,t);
}
void oracle_destroy(int p,void *b) {if(p)fixture_nes_destroy(b);else fixture_genesis_destroy(b);}
void oracle_push(int p,void *b,const int16_t *s,int n) {if(p)fixture_nes_push(b,s,n);else fixture_genesis_push(b,s,n);}
void oracle_pull(int p,void *b,int16_t *s,int n) {if(p)fixture_nes_pull(b,s,n);else fixture_genesis_pull(b,s,n);}
void oracle_observe(int p,void *b,oracle_state *s) {if(p)fixture_nes_observe(b,s);else fixture_genesis_observe(b,s);}
void oracle_reset(int p,void *b) {if(p)fixture_nes_reset(b);else fixture_genesis_reset(b);}
