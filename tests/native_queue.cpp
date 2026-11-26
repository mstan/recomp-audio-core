#include <recomp_audio_native_queue.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>
#include <numeric>
using namespace recomp_audio_native;
struct Sink {
    std::vector<int16_t> delivered; unsigned queued=0,waits=0,attempts=0;
    bool fail=false,full_once=false;
    unsigned count(){return queued;}
    Push try_push(const int16_t* p,unsigned n){
        ++attempts;if(fail)return Push::Failed;
        if(full_once){full_once=false;return Push::Full;}
        delivered.insert(delivered.end(),p,p+2*n);queued+=n;return Push::Done;
    }
    void wait(){++waits;queued=queued>512?queued-512:0;}
};
int main(){
    StereoRing<4> ring;int16_t a[]={1,2,3,4,5,6,7,8};int16_t out[8]{};
    assert(ring.push(a,3));assert(!ring.push(a,2));assert(ring.pull(out,2)==2);
    assert(out[0]==1&&out[3]==4);assert(ring.push(a,3));
    assert(ring.pull(out,4)==4);int16_t expected[]={5,6,1,2,3,4,5,6};
    for(int i=0;i<8;++i)assert(out[i]==expected[i]);assert(ring.count==0);
    ring.push(a,4);ring.reset();assert(ring.count==0&&ring.read==0&&ring.write==0);
    std::vector<int16_t> input(5000);std::iota(input.begin(),input.end(),-2500);
    unsigned at=0;auto source=[&](int16_t*p,unsigned cap){unsigned n=std::min(cap,unsigned(input.size()/2-at));std::copy_n(input.data()+at*2,n*2,p);at+=n;return n;};
    Sink sink;sink.full_once=true;bool error=false;
    assert(drain(sink,source,true,500,error)<=500);assert(!error&&at==2500);
    assert(sink.delivered==input&&sink.waits>0);
    Sink failed;failed.fail=true;at=0;error=false;drain(failed,source,true,500,error);assert(error&&failed.attempts==1);
    Sink full;full.full_once=true;at=0;error=false;drain(full,source,false,500,error);assert(error&&full.waits==0);
}
