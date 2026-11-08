# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise the recorded Vulkan sample-counter and empty-draw branches without a GPU or a game."""
from pathlib import Path
import subprocess
import os
import tempfile
repo = Path(__file__).resolve().parents[2]
_temporary = tempfile.TemporaryDirectory(prefix='stitch-query-check-')
work = Path(_temporary.name)
out = work/'empty-query-regression'
out.mkdir(exist_ok=True)
vk = repo/'src/video_core/renderer_vulkan'
samples = (vk/'vk_query_cache.cpp').read_text().split('class SamplesStreamer :')[1]
raster = (vk/'vk_rasterizer.cpp').read_text()
def method(source, signature):
    start=source.index(signature)
    body=source.index('{', start)
    depth=1
    for i in range(body+1,len(source)):
        depth += (source[i]=='{') - (source[i]=='}')
        if depth==0: return source[start:i+1].replace(' override','')
    raise ValueError(signature)
signatures=['bool CanReserveInCurrentBank()', 'void StartCounter()', 'void PauseCounter()',
            'void ResetCounter()', 'void CloseCounter()', 'size_t WriteCounter(',
            'void ReserveHostQuery()', 'void AbandonCurrentQuery()']
empty=raster.split('        if (groups == 0) {',1)[1].split('            for (size_t i = 0; i < blockers.size(); ++i)',1)[0]
unconditional=raster[raster.index('        const bool unconditional{'):raster.index('        static const bool capture_empty')]
runtime=method((vk/'vk_query_cache.cpp').read_text(),'bool QueryCacheRuntime::CanBeginSamplesQueryInPass()')
fixture=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <vector>
using u32=uint32_t;using u64=uint64_t;using DAddr=u64;
namespace Settings { bool high{}; bool IsGPULevelHigh(){return high;} }
namespace VideoCommon { enum class QueryType {ZPassPixelCount64}; struct QueryFlagBits {enum {IsOrphan=1,IsFinalValueSynced=2,HasTimestamp=4};}; }
constexpr int VK_QUERY_CONTROL_PRECISE_BIT=1;
#define LOG_WARNING(...) ((void)0)
struct SamplesQueryBank { static constexpr size_t BANK_SIZE=256; size_t next_bank{}, used{},refs{};
  bool IsClosed() const{return used==BANK_SIZE;} void AddReference(size_t n){refs+=n;} };
struct BankPool { std::deque<SamplesQueryBank> banks; SamplesQueryBank& GetBank(size_t id){return banks.at(id);} };
struct Slot {u64 count{};bool precise{};};
namespace vk { struct CommandBuffer {
  std::vector<Slot>& slots; int& active;
  void BeginQuery(size_t bank,u32 index,int flags){assert(active==-1); active=bank*256+index; assert(active==int(slots.size())); slots.push_back({0,bool(flags)});}
  void EndQuery(size_t bank,u32 index){assert(active==int(bank*256+index));active=-1;}
};}
struct Scheduler { bool pass=true; int active=-1; std::vector<Slot> slots;
  bool IsRenderPassActive()const{return pass;}
  void Record(auto fn){assert(pass);fn(vk::CommandBuffer{slots,active});}
  void Draw(unsigned n){assert(active>=0); auto& s=slots.at(active); s.count=s.precise?s.count+n:u64(s.count!=0 || n!=0);}
};
struct Query {size_t start_bank_id{},size_banks{},start_slot{},size_slots{};u64 value{},guest_address{};u32 flags=1;};
struct Raster {void SyncOperation(std::function<void()> fn){fn();}};
struct Stream {
  Scheduler& scheduler; Raster rr,*rasterizer=&rr; BankPool bank_pool;
  bool has_started{},accumulation_since_last_sync{};size_t bank_bound_resets{},num_slots_used{},current_query_id{},current_bank_slot{},current_bank_id{},current_query_pool{};
  size_t first_accumulation_checkpoint=SIZE_MAX,last_accumulation_checkpoint{},amend_value{},accumulation_value{};
  SamplesQueryBank* current_bank=nullptr; Query* current_query=nullptr;
  std::vector<size_t> pending_sync,pending_flush_queries; std::deque<Query> queries;
  size_t BuildQuery(){queries.emplace_back();return queries.size()-1;} Query* GetQuery(size_t id){return &queries.at(id);} void Free(size_t){}
  void ApplyBankOp(Query*q,auto fn){size_t left=q->size_slots,start=q->start_slot;for(size_t b=0;b<q->size_banks;++b){auto n=std::min(left,256-start);fn(&bank_pool.GetBank(q->start_bank_id+b),start,n);left-=n;start=0;}}
  size_t ReserveBankSlot(){if(!current_bank||current_bank->IsClosed()){if(current_bank)current_bank->next_bank=current_bank_id+2;current_bank_id=bank_pool.banks.size();bank_pool.banks.emplace_back();current_bank=&bank_pool.banks.back();current_query_pool=current_bank_id;} return current_bank_slot=current_bank->used++;}
  METHODS
  u64 Report(){auto* q=GetQuery(WriteCounter(0x1000,false,0,{}));u64 result=0;for(size_t i=0;i<q->size_slots;++i)result+=scheduler.slots.at(q->start_bank_id*256+q->start_slot+i).count;return result;}
};
struct RuntimeImpl{Scheduler& scheduler;Stream& sample_streamer;bool hcr_is_set{};};
struct QueryCacheRuntime{RuntimeImpl* impl;bool CanBeginSamplesQueryInPass()const;};
RUNTIME
namespace Maxwell {struct RenderEnable{enum class Override{UseRenderEnable,AlwaysRender,NeverRender};enum class Mode{True,Conditional};};}
namespace GPU::Logging { bool IsActive(){return false;} }
namespace VideoCommon::RenderTrace {bool SampleFrame(int){return false;}}
namespace Common::DkbFrameTrace {bool Enabled(){return false;}}
struct State {
   Scheduler scheduler;Stream s{scheduler};RuntimeImpl impl{scheduler,s};QueryCacheRuntime query_cache_runtime{&impl};
   struct { Maxwell::RenderEnable::Override render_enable_override=Maxwell::RenderEnable::Override::UseRenderEnable;
            struct{Maxwell::RenderEnable::Mode mode=Maxwell::RenderEnable::Mode::True;} render_enable; u32 zpass_pixel_count_enable=1,transform_feedback_enabled=0;}regs;
   struct Q{Stream& s;void CounterEnable(VideoCommon::QueryType,bool on){if(on)s.StartCounter();else s.PauseCounter();}}query_cache{s};
   struct{bool Capturing(){return false;}}frame_dump;
   bool mesh_empty_use_baseline{},probe{},capture_empty{},submitted{};std::array<int,10> mesh_empty_blockers{};int render_trace_frame{},mesh_empty_bypassed{},slow{};
   void HandleTransformFeedback(){}
   void Empty(){
      UNCONDITIONAL
      EMPTY
      ++slow;
      // The existing mesh configuration requests an outside-render-pass context, then requests
      // a framebuffer and enables counting in PrepareDraw; the zero draw records no samples.
      if(scheduler.pass)s.PauseCounter();
      scheduler.pass=true;
      if(regs.zpass_pixel_count_enable)s.StartCounter();
   }
   void Draw(int n){scheduler.pass=true;s.StartCounter();scheduler.Draw(n);}
};
void test(bool baseline,bool precise){
   Settings::high=precise;
   {State x;x.mesh_empty_use_baseline=baseline;x.Draw(5);x.Empty();x.Draw(7);assert(x.s.Report()==(precise?12:2));assert(x.scheduler.slots.size()==2);assert(x.mesh_empty_bypassed==!baseline);}
   {State x;x.mesh_empty_use_baseline=baseline;x.Draw(5);x.s.ResetCounter();x.Empty();assert(x.mesh_empty_bypassed==!baseline);assert(x.s.current_query->size_slots==1);assert(x.s.Report()==0);}
   {State x;x.mesh_empty_use_baseline=baseline;x.Draw(5);x.Empty();assert(x.s.Report()==(precise?5:1));x.Empty();assert(x.slow==2*int(baseline));assert(x.s.current_query->size_slots==3);assert(x.s.Report()==(precise?5:1));}
   {State x;x.mesh_empty_use_baseline=baseline;x.Draw(5);x.regs.zpass_pixel_count_enable=0;x.Empty();assert(!x.s.has_started);assert(x.s.Report()==(precise?5:1));}
   {State x;x.s.ResetCounter();x.Draw(5);x.regs.render_enable.mode=Maxwell::RenderEnable::Mode::Conditional;x.Empty();assert(x.slow==1);}
   {State x;x.Draw(5);x.impl.hcr_is_set=true;x.Empty();assert(x.slow==1);}
   {State x;x.Draw(5);x.regs.transform_feedback_enabled=1;x.Empty();assert(x.slow==1);}
   {State x;x.Empty();assert(x.slow==1);x.s.PauseCounter();x.scheduler.pass=false;x.Empty();assert(x.slow==2);}
   {State x;x.mesh_empty_use_baseline=baseline;x.Draw(3);for(int i=1;i<256;i++)x.Empty();assert(x.s.current_query->size_slots==256);assert(x.mesh_empty_bypassed==(baseline?0:255));x.Empty();assert(x.slow>0);assert(x.s.current_query->size_slots==257);assert(x.s.bank_bound_resets==0);x.s.CloseCounter();assert(x.s.bank_bound_resets==1);assert(x.s.Report()==0);}
}
int main(){for(bool baseline : {false,true})for(bool precise : {false,true})test(baseline,precise);std::puts("PASS precise and boolean query slots, reset/report, conditional/predicate, transform feedback, 256-slot rollover, submission bound; both marker modes");}
'''
source=fixture.replace('  METHODS','\n'.join(method(samples,s) for s in signatures)).replace('RUNTIME',runtime).replace('UNCONDITIONAL',unconditional).replace('      EMPTY',empty)
for name,text in [('fixed',source),('merged',source.replace('query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);','if (!count_samples) query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);'))]:
    cpp=out/(name+'.cpp'); bin=out/name; cpp.write_text(text)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20',str(cpp),'-o',str(bin)],check=True)
    r=subprocess.run([bin],cwd=out,capture_output=True,text=True)
    if name=='fixed':print(r.stdout+r.stderr);assert r.returncode==0
    else:print('Negative merged-query result:',r.stderr.strip());assert r.returncode and 'Report()' in r.stderr
