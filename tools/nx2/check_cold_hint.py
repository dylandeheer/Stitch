# SPDX-License-Identifier: GPL-3.0-or-later

"""Replay the production cold hint and reject forced Linux swap-out. Run on Linux."""
import os
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[2]
s=(repo/'src/common/host_memory.cpp').read_text()
a=s.index('void HostMemory::HintCold(');b=s.index('\nvoid HostMemory::ClearBackingRegion',a)
method=s[a:b]
fixture=r'''#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>
#include <sys/mman.h>
using u8=uint8_t;
struct Call{void* address;size_t length;int advice;};
std::vector<Call> calls;
int observed_madvise(void* p,size_t n,int hint){calls.push_back({p,n,hint});return 0;}
#define madvise observed_madvise
struct HostMemory{u8* backing_base{};u8* virtual_base{};size_t virtual_size{};
 void HintCold(size_t,size_t,std::optional<size_t>)const;};
METHOD
int main(){
 std::array<u8,65536> backing{},view{};
 HostMemory m{backing.data(),view.data(),view.size()};
 m.HintCold(4096,0,8192);assert(calls.empty());
 m.backing_base=nullptr;m.HintCold(0,4096,0);assert(calls.empty());m.backing_base=backing.data();
 m.HintCold(4096,4096,8192);assert(calls.size()==2);
 assert(calls[0].address==backing.data()+4096&&calls[1].address==view.data()+8192);
 for(const auto& c:calls)assert(c.length==4096&&c.advice==MADV_COLD);
 calls.clear();m.HintCold(0,4096,std::nullopt);assert(calls.size()==1&&calls[0].advice==MADV_COLD);
 calls.clear();m.HintCold(0,4096,view.size()-4096);assert(calls.size()==2);
 calls.clear();m.HintCold(0,4096,view.size());assert(calls.size()==1);
}
'''
with tempfile.TemporaryDirectory(prefix='stitch-cold-hint-') as t:
 for name,body in [('fixed',method),('forced_pageout',method.replace('madvise(address, length, MADV_COLD)','madvise(address, length, MADV_PAGEOUT)'))]:
  src=Path(t)/(name+'.cpp');exe=Path(t)/name;src.write_text(fixture.replace('METHOD',body))
  subprocess.run([os.environ.get('CXX','c++'),'-std=c++20',str(src),'-o',str(exe)],check=True)
  done=subprocess.run([exe],capture_output=True,text=True)
  if name=='fixed':assert done.returncode==0,done.stderr;print('PASS actual HostMemory::HintCold: Linux MADV_COLD, backing/view bounds, zero/null, optional view')
  else:assert done.returncode!=0;print('PASS negative forced-PAGEOUT variant rejected')
