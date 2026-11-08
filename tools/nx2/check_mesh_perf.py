# SPDX-License-Identifier: GPL-3.0-or-later

"""Check production presentation marker ordering and timestamp command selection with a recorder."""
from pathlib import Path
import os, subprocess, tempfile
repo=Path(__file__).resolve().parents[2]
_temporary=tempfile.TemporaryDirectory(prefix='stitch-mesh-perf-check-')
work=Path(_temporary.name)
out=work/'mesh-perf-regression'; out.mkdir(exist_ok=True)
base=repo/'src/video_core/renderer_vulkan'
src=(base/'vk_scheduler.cpp').read_text(); header=(base/'vk_scheduler.h').read_text()
def method(text, signature):
    start=text.index(signature); begin=text.index('{',start); n=1
    for i in range(begin+1,len(text)):
        n+=(text[i]=='{')-(text[i]=='}')
        if n==0:return text[start:i+1]
    raise ValueError(signature)
fixture=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>
using u32=uint32_t;using u64=uint64_t;
#define LOG_INFO(...) ((void)0)
namespace Common::DkbFrameTrace {bool on=false;bool Enabled(){return on;}}
constexpr int VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT=1;
namespace vk {struct CommandBuffer{std::vector<u32>& events;void WriteTimestamp(int,int,u32 q){events.push_back(q);}};}
struct Scheduler{
    static bool MeshPerfRequested();
    std::atomic<bool> mesh_perf_active{};
    bool MeshPerfActive(){return mesh_perf_active.load(std::memory_order_relaxed);}
    enum class WorkKind {Guest,MeshCompute,MeshGraphics};struct Work {u64 hash,frag;std::array<u32,3> size;WorkKind kind;};
    bool trace_allow_next_frame=false,trace_gpu_recording=false;u64 trace_frame_end=0;
    std::optional<int> trace_work_pool;static constexpr u32 TraceMaxWork=4;std::array<Work,4>trace_work_items{};u32 trace_work_count{};
    std::vector<std::function<void(vk::CommandBuffer)>> commands;
    std::vector<u32> events;
    void Record(auto cb){commands.push_back(cb);}
    void Run(){for(auto& c:commands)c(vk::CommandBuffer{events});commands.clear();}
    void MarkFrameEnd(u64);
    TRACE
};
REQUEST
MARK
int main(int argc,char**argv){
    const char* marker=std::getenv("EDEN_NX2_MESH_PERF_FILE");
    const bool dk=argc>1;Common::DkbFrameTrace::on=dk;
    Scheduler s;auto mark=[&](int f){s.MarkFrameEnd(f);s.Run();};
    mark(1);assert(!s.MeshPerfActive() && s.trace_allow_next_frame==dk);
    std::ofstream(marker)<<"arm";mark(29);assert(!s.MeshPerfActive());s.MarkFrameEnd(30);
    assert(s.MeshPerfActive() && s.trace_allow_next_frame==dk);
    s.Run();assert(s.trace_frame_end==30 && s.trace_allow_next_frame);
    auto op=[&](auto kind){s.TraceGpuWork(vk::CommandBuffer{s.events},10,11,{6,1,0},[&]{s.events.push_back(99);},kind);};
    using K=Scheduler::WorkKind;
    op(K::MeshCompute);assert(s.events==std::vector<u32>{99});
    s.events.clear();s.trace_gpu_recording=true;op(K::MeshCompute);assert(s.trace_work_count==0);
    s.trace_work_pool.emplace(1);s.events.clear();
    op(K::Guest);assert(s.trace_work_count==unsigned(dk));op(K::MeshCompute);op(K::MeshGraphics);
    const std::vector<u32> want=dk ? std::vector<u32>{0,99,1,2,99,3,4,99,5} : std::vector<u32>{99,0,99,1,2,99,3};
    assert(s.events==want && s.trace_work_items[dk].kind==K::MeshCompute && s.trace_work_items[dk+1].kind==K::MeshGraphics);
    s.trace_work_count=4;s.events.clear();op(K::MeshGraphics);assert(s.events==std::vector<u32>{99} && s.trace_work_count==4);
    std::filesystem::remove(marker);s.MarkFrameEnd(60);assert(!s.MeshPerfActive() && s.trace_allow_next_frame);
    s.Run();assert(s.trace_frame_end==60 && s.trace_allow_next_frame==dk);
    mark(61);assert(s.trace_frame_end==(dk?61:60));
    std::puts(dk?"PASS original DKB timing preserved with mesh capture disarmed":"PASS mesh-only marker arm/disarm, worker order, paired timestamps, pool/limit fallback");
}
'''
t=header.index('    template <typename Func>\n    void TraceGpuWork')
code=fixture.replace('    TRACE',method(header[t:], 'template <typename Func>')).replace('REQUEST',method(src,'bool Scheduler::MeshPerfRequested()')).replace('MARK',method(src,'void Scheduler::MarkFrameEnd('))
cpp=out/'record.cpp';cpp.write_text(code)
exe=out/'record';subprocess.run([os.environ.get('CXX','c++'),'-std=c++20',str(cpp),'-o',str(exe)],check=True)
env=dict(os.environ,EDEN_NX2_MESH_PERF='1',EDEN_NX2_MESH_PERF_FILE=str(out/'marker'))
for args in ([],['dkb']):subprocess.run([exe,*args],env=env,check=True)
