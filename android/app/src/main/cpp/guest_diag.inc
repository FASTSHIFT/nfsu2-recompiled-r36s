// Guest state dump for hangs that leave every host thread healthy (see the note above noteFileOpen in
// guest_runtime.cpp). Included after the thread, file and kernel code it reads.
//
// Triggers (checked every 2 s by the stall watch):
//   - a file named dump.now next to ports/nfsu2/ (override: NFSU2_DUMP_FILE); it is deleted when read, so
//     `touch /roms/ports/nfsu2/dump.now` over ssh writes one dump into log.txt;
//   - automatically, when the game has opened no file for 45 s and the last file was not a STREAM* bundle (the race
//     streams track data all the time). At most 4 automatic dumps, 120 s apart.
// NFSU2_NO_DIAG=1 switches both off. Every line starts with "[NFSU2 DUMP]".
namespace {
std::string procValue(const char* path,const char* key){
    FILE* f=std::fopen(path,"r");if(!f)return "?";
    char line[256];std::string value="?";const size_t n=std::strlen(key);
    while(std::fgets(line,sizeof line,f))if(!std::strncmp(line,key,n)){
        std::string text=line+n;while(!text.empty()&&(text.back()=='\n'||text.back()==' '))text.pop_back();
        size_t start=text.find_first_not_of(" \t:");value=start==std::string::npos?text:text.substr(start);break;
    }
    std::fclose(f);return value;
}
}  // namespace
// The guest functions of a host backtrace (the lifted code keeps its sub_XXXXXXXX names), innermost first.
std::string guestChain(void* const* frames,int depth){
    std::string text;
#if !defined(__ANDROID__)
    if(depth<=0)return text;
    char** names=backtrace_symbols(frames,depth);
    if(!names)return text;
    for(int i=0;i<depth;++i){
        const std::string name=names[i];const auto open=name.find('('),plus=name.find('+',open==std::string::npos?0:open);
        if(open==std::string::npos||plus==std::string::npos||plus<=open+1)continue;
        const std::string symbol=name.substr(open+1,plus-open-1);
        if(symbol.rfind("sub_",0)!=0)continue;
        if(!text.empty())text+=" < ";
        text+=symbol.substr(4);
    }
    std::free(names);
#else
    (void)frames;
#endif
    return text;
}
// Real-time signal handler: the receiving thread writes its own host backtrace (guest function names included) to the log.
void backtraceHandler(int){
#if !defined(__ANDROID__)
    void* frames[40];const int saved=errno;
    char name[32]={};::prctl(PR_GET_NAME,name);
    char head[96];const int n=std::snprintf(head,sizeof head,"[NFSU2 DUMP]   backtrace of thread '%s' tid %ld:\n",name,static_cast<long>(::syscall(SYS_gettid)));
    if(n>0){ssize_t w=::write(2,head,size_t(n));(void)w;}
    const int depth=::backtrace(frames,40);
    ::backtrace_symbols_fd(frames,depth,2);
    errno=saved;
#endif
}
std::atomic<bool> backtraceReady{false};
void installBacktraceSignal(){
#if !defined(__ANDROID__)
    static std::once_flag once;
    std::call_once(once,[]{
        void* warm[4];::backtrace(warm,4);   // load libgcc's unwinder now, not inside the signal handler
        struct sigaction action{};action.sa_handler=backtraceHandler;action.sa_flags=SA_RESTART;sigemptyset(&action.sa_mask);
        backtraceReady=::sigaction(SIGRTMIN+5,&action,nullptr)==0;
    });
#endif
}
// Sends the backtrace signal to every other thread, one at a time.
void dumpThreadBacktraces(){
#if !defined(__ANDROID__)
    if(std::getenv("NFSU2_DUMP_NO_BT"))return;
    installBacktraceSignal();
    if(!backtraceReady)return;
    NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","[NFSU2 DUMP] host backtraces of every thread (guest functions are sub_XXXXXXXX):");
    const pid_t self=static_cast<pid_t>(::syscall(SYS_gettid));
    if(DIR* dir=::opendir("/proc/self/task")){
        std::vector<long> tids;
        while(dirent* entry=::readdir(dir)){const long tid=std::atol(entry->d_name);if(tid>0&&tid!=self)tids.push_back(tid);}
        ::closedir(dir);
        for(long tid:tids){::syscall(SYS_tgkill,::getpid(),tid,SIGRTMIN+5);std::this_thread::sleep_for(std::chrono::milliseconds(60));}
    }
#endif
}
void dumpGuestState(const char* reason){
    static std::mutex dumping;
    std::unique_lock<std::mutex> once(dumping,std::try_to_lock);
    if(!once.owns_lock())return;
    std::vector<std::string> out;
    auto add=[&](const char* format,auto... values){char line[480];std::snprintf(line,sizeof line,format,values...);out.emplace_back(line);};
    const int64_t now=steadyMs(),opened=lastFileOpenMs.load();
    add("=== guest state dump: %s ===",reason);
    add("frame %u; last Present %lld ms ago; last file open %lld ms ago (%s); allocation failures %llu; thread exceptions %llu",
        getD3D9PresentCount(),static_cast<long long>(now-lastPresentMs.load()),static_cast<long long>(opened?now-opened:-1),lastOpenedFile().c_str(),
        static_cast<unsigned long long>(allocFailures.load()),static_cast<unsigned long long>(threadExceptions.load()));
    add("memory: RSS %s, swap %s; system available %s, swap free %s",procValue("/proc/self/status","VmRSS:").c_str(),procValue("/proc/self/status","VmSwap:").c_str(),
        procValue("/proc/meminfo","MemAvailable:").c_str(),procValue("/proc/meminfo","SwapFree:").c_str());
    {
        uint64_t next=0,serving=0;unsigned depth=0;bool owned=false;machine.snapshot(next,serving,depth,owned);
        add("machine lock: %s, %llu thread(s) holding or queued; %llu acquisitions, %llu contended; sync generation %llu, %u waiter(s)",owned?"held":"free",
            static_cast<unsigned long long>(next>serving?next-serving:0),static_cast<unsigned long long>(statMachineLocks.load()),
            static_cast<unsigned long long>(statMachineWaits.load()),static_cast<unsigned long long>(guestSyncGeneration.load()),guestSyncWaiters);
    }
    {
        std::lock_guard<std::mutex> l(fileNotesMutex);
        const uint64_t count=std::min<uint64_t>(fileNoteCount,16);
        add("last %llu file opens (oldest first):",static_cast<unsigned long long>(count));
        for(uint64_t i=fileNoteCount-count;i<fileNoteCount;++i){
            const auto& note=fileNotes[i%fileNotes.size()];
            add("  frame %-6u %6lld ms ago  thread %-2u %s",note.frame,static_cast<long long>(now-note.ms),note.thread,note.name);
        }
        // Which guest code opened them: the chain of each distinct (file, chain) among the last opens.
        std::set<std::string> shown;
        for(uint64_t i=fileNoteCount;i>fileNoteCount-std::min<uint64_t>(fileNoteCount,24)&&shown.size()<6;--i){
            const auto& note=fileNotes[(i-1)%fileNotes.size()];
            const std::string chain=guestChain(note.frames,note.depth);
            if(chain.empty()||!shown.insert(chain).second)continue;
            add("  opened by (frame %u, %s): %s",note.frame,note.name,chain.c_str());
        }
    }
    {
        unsigned used=0;
        for(uint32_t i=0;i<syncStats.size();++i){
            const auto& s=syncStats[i];
            if(!s.sets&&!s.waits&&!s.resets)continue;
            if(!used++)add("event history (since start; ages in ms):");
            auto age=[&](int64_t t){return t?static_cast<long long>(now-t):-1ll;};
            add("  %08x sets %llu (last %lld ago by thread %u) resets %llu | waits %llu, woke %llu, timed out %llu (last wake %lld ago, thread %u; last wait began %lld ago, thread %u)",0xf0040000u+i,
                static_cast<unsigned long long>(s.sets.load()),age(s.lastSetMs),s.lastSetThread.load(),static_cast<unsigned long long>(s.resets.load()),
                static_cast<unsigned long long>(s.waits.load()),static_cast<unsigned long long>(s.wakes.load()),static_cast<unsigned long long>(s.timeouts.load()),
                age(s.lastWakeMs),s.lastWakeThread.load(),age(s.lastWaitMs),s.lastWaitThread.load());
        }
    }
    // Guest tables change only under the machine: take it (waiting up to 3 s) so the maps are stable.
    std::map<uint32_t,std::pair<bool,bool>> eventState;
    bool gotMachine=false;
    for(int attempt=0;attempt<600&&!gotMachine;++attempt){
        gotMachine=machine.try_lock();
        if(!gotMachine)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(!gotMachine)add("guest tables skipped: the machine lock was busy for 3 s");
    else{
        try{
            add("guest threads (%zu):",threadHandles.size());
            for(const auto& [handle,thread]:threadHandles)
                add("  thread %-3u entry %08x suspended %u done %d exit %u%s",thread->id,thread->start,thread->suspended,int(thread->done),thread->exitCode,thread->timer?" (timer worker)":"");
            add("events (%zu):",eventHandles.size());
            for(const auto& [handle,event]:eventHandles){
                eventState[handle]={event->manual,event->signaled};
                add("  %08x %s %s",handle,event->manual?"manual-reset":"auto-reset",event->signaled?"SIGNALED":"not signaled");
            }
            {
                std::lock_guard<std::mutex> l(guestMutexState);
                add("mutexes (%zu):",mutexHandles.size());
                for(const auto& [handle,mutex]:mutexHandles)add("  %08x owner thread %u depth %u%s",handle,mutex->owner,mutex->depth,mutex->abandoned?" abandoned":"");
            }
            {
                std::lock_guard<std::mutex> l(criticalMapMutex);
                unsigned locked=0;
                for(const auto& entry:criticalSections){
                    const uint32_t address=entry.first;
                    if(!memory||!memory->readable(address,24))continue;
                    const uint32_t count=read32(address+8);
                    if(count&&count<1000){++locked;add("  locked critical section %08x owner thread %u, recursion %u",address,read32(address+12),count);}
                }
                add("critical sections: %zu known, %u locked",criticalSections.size(),locked);
            }
            add("timers: %zu",timerHandles.size());
            if(heaps){
                const auto s=heaps->stats();
                add("guest heaps: live %llu KB in %llu blocks; free %llu KB in %llu pieces, largest %u KB",static_cast<unsigned long long>(s.live/1024),
                    static_cast<unsigned long long>(s.blocks),static_cast<unsigned long long>(s.freeTotal/1024),static_cast<unsigned long long>(s.freePieces),s.largestFree/1024);
            }
            add("open guest files (%zu):",fileHandles.size());
            for(const auto& [handle,fd]:fileHandles){
                char link[64],path[320]={};std::snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
                const ssize_t n=::readlink(link,path,sizeof path-1);
                struct stat status{};const off_t position=::lseek(fd,0,SEEK_CUR);
                const bool sized=::fstat(fd,&status)==0;
                ReadNote read;{std::lock_guard<std::mutex> l(fileNotesMutex);auto it=readNotes.find(handle);if(it!=readNotes.end())read=it->second;}
                const char* base=n>0?std::strrchr(path,'/'):nullptr;
                add("  %08x %s pos %lld of %lld; %llu reads, %llu KB; last read asked %u got %u, %lld ms ago",handle,base?base+1:"?",static_cast<long long>(position),
                    static_cast<long long>(sized?status.st_size:-1),static_cast<unsigned long long>(read.reads),static_cast<unsigned long long>(read.bytes/1024),
                    read.asked,read.got,static_cast<long long>(read.ms?now-read.ms:-1));
            }
        }catch(const std::exception& error){add("guest tables incomplete: %s",error.what());}
        machine.unlock();
    }
    {
        std::lock_guard<std::mutex> lock(threadTracesMutex);
        add("guest thread waits (%zu):",threadTraces.size());
        for(const auto& [id,trace]:threadTraces){
            const char* wait=trace->wait.load();const uint32_t handle=trace->handle.load();
            std::string held;for(const auto& slot:trace->held)if(uint32_t cs=slot.load())held+=" "+hex(cs);
            std::string state;
            if(wait&&std::strstr(wait,"(event)")){auto it=eventState.find(handle);if(it!=eventState.end())state=it->second.second?" [SIGNALED]":" [not signaled]";}
            add("  thread %-3u at %08x %s %08x%s; holds:%s",id,trace->func.load(),wait?wait:"running",wait?handle:0u,state.c_str(),held.empty()?" none":held.c_str());
        }
    }
    add("=== end of guest state dump ===");
    for(const auto& line:out)NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","[NFSU2 DUMP] %s",line.c_str());
    dumpThreadBacktraces();
    NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","[NFSU2 DUMP] === end of backtraces ===");
}
void diagTick(){
    static const bool off=std::getenv("NFSU2_NO_DIAG")!=nullptr;
    if(off)return;
    static const std::filesystem::path trigger=[]{
        if(const char* configured=std::getenv("NFSU2_DUMP_FILE");configured&&*configured)return std::filesystem::path(configured);
        std::string root=hostGameRoot;while(root.size()>1&&root.back()=='/')root.pop_back();
        return std::filesystem::path(root).parent_path()/"dump.now";
    }();
    std::error_code error;
    if(std::filesystem::exists(trigger,error)){std::filesystem::remove(trigger,error);dumpGuestState("trigger file");return;}
    static int automatic=0;static int64_t lastAutomatic=0;
    const int64_t now=steadyMs(),opened=lastFileOpenMs.load();
    if(!opened||automatic>=4||guestPaused.load()||now-opened<45000||(lastAutomatic&&now-lastAutomatic<120000))return;
    const std::string name=lastOpenedFile();
    if(name.rfind("STREAM",0)==0||name.rfind("stream",0)==0)return;
    lastAutomatic=now;++automatic;
    dumpGuestState(("no file opened for "+std::to_string((now-opened)/1000)+" s after "+name).c_str());
}
