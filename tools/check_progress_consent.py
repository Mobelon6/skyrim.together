"""Exercise the actual consent queue, identity checks, directions, and effect ordering."""
from pathlib import Path
import subprocess
import sys
import tempfile

src = Path(sys.argv[1], 'Code/client/Services')
header = (src/'OverlayService.h').read_text()
fields = header[header.index('    using ProgressKey ='):header.index('    void ClearProgress()')]
text = (src/'Generic/ProgressConsent.cpp').read_text()
implementation = text[text.index('void OverlayService::QueueProgress'):text.index('void OverlayService::UpdateProgress')]
harness = """
#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <cstdint>
struct Party {bool in=true;bool IsInParty(){return in;}};
struct World {Party p;Party& GetPartyService(){return p;}};
struct UI {void ExecuteAsync(const char*){}void SetUIVisible(bool){}};
struct OverlayService {
    World m_world; UI ui;UI* m_pOverlay=&ui;UI* m_gameOverlayClient=&ui;
    std::map<unsigned,unsigned> m_peaceSession;
    FIELDS
    void QueueProgress(bool,uint32_t,uint32_t,uint32_t,const char*,std::function<void()>,std::function<bool()> = {},uint32_t = 0,uint32_t = 0) noexcept;
    void AnswerProgress(uint32_t,bool) noexcept;
    bool IsPersonalQuest(uint32_t) const noexcept;
    void ClearProgress() noexcept;
};
"""
checks = """
int main() {
    OverlayService s;
    std::vector<int> applied;
    auto add=[&](bool q,unsigned form,unsigned session,int stage){
        s.QueueProgress(q,form,0,session,"encounter",[&,stage]{applied.push_back(stage);});
    };
    add(true,50,7,10);add(true,50,7,20);add(false,50,7,100);
    assert(s.m_progress.size()==2 && s.IsPersonalQuest(50) && applied.empty());
    auto first=s.m_progress.front().Token;
    s.AnswerProgress(first,true);assert(applied.empty()); // Not yet displayed.
    s.m_showingProgress=true;s.AnswerProgress(first+1,true);assert(applied.empty());
    s.AnswerProgress(first,true);s.AnswerProgress(first,true);add(true,50,7,200);
    assert((applied==std::vector<int>{10,20,200}) && !s.IsPersonalQuest(50));
    s.m_showingProgress=true;s.AnswerProgress(s.m_progress.front().Token,false);
    add(false,50,7,101);assert(applied.size()==3 && s.m_progress.empty());
    add(true,50,8,10);s.m_showingProgress=true;s.AnswerProgress(s.m_progress.front().Token,false);
    assert(s.IsPersonalQuest(50));add(true,50,8,20);assert(applied.size()==3);
    add(true,99,4,3);auto stale=s.m_progress.front().Token;s.ClearProgress();
    assert(!s.IsPersonalQuest(50));s.AnswerProgress(stale,true);assert(applied.size()==3);
    bool running=false;
    auto run=[&] {return running;};
    s.QueueProgress(true,61,2,1,"unrelated",[]{assert(false);},run);
    assert(s.m_progress.empty());
    s.QueueProgress(true,61,2,1,"actor",[&]{running=true;},[&]{return !running;},81,12);
    s.QueueProgress(true,61,2,1,"actor",[&]{running=false;},run,81,12);
    s.QueueProgress(false,81,2,12,"actor",[&]{applied.push_back(77);});
    assert(s.m_progress.size()==1 && s.IsPersonalQuest(61));
    s.m_showingProgress=true;s.AnswerProgress(s.m_progress.front().Token,true);
    assert(!running && applied.back()==77 && !s.IsPersonalQuest(61));
    s.QueueProgress(false,81,2,12,"actor",[&]{applied.push_back(88);});
    assert(s.m_progress.empty() && applied.back()==88);
    s.m_world.p.in=false;add(true,99,5,8);assert(s.m_progress.empty());
}
"""
with tempfile.TemporaryDirectory() as folder:
    exe = Path(folder, "consent")
    subprocess.run(["g++", "-std=c++20", "-x", "c++", "-", "-o", str(exe)], check=True,
                   input=harness.replace("FIELDS", fields)+implementation+checks, text=True)
    subprocess.run([str(exe)],check=True)
print("PASS queued stages, clicked token, duplicate response, accept, decline, next run, session cleanup")
