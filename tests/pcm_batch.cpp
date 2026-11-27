// SPDX-License-Identifier: MIT
#include "recomp_audio_pcm_batch.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>
int main() {
  recomp_audio_native::StereoBatch<64> batch;
  std::vector<int16_t> pcm(512),actual;
  for(unsigned n=0;n<pcm.size();++n)pcm[n]=int16_t(n*31-7900);
  std::vector<uint32_t> chunks;
  auto sink=[&](const int16_t *p,uint32_t frames){chunks.push_back(frames);actual.insert(actual.end(),p,p+frames*2);};
  for(unsigned n=0;n<256;n+=8)batch.push(pcm.data()+n*2,8,sink);
  assert(chunks==std::vector<uint32_t>({64,64,64,64}) && actual==pcm);
  batch.push(pcm.data(),8,sink);batch.flush(sink);
  assert(chunks.back()==8 && actual.size()==528);
  batch.push(pcm.data(),8,sink);batch.push(pcm.data()+16,128,sink);
  assert(chunks[chunks.size()-2]==8 && chunks.back()==128);
  batch.push(pcm.data(),8,sink);batch.reset();const auto before=actual;batch.flush(sink);
  assert(actual==before);
}
