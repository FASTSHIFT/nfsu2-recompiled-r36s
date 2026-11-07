std::string guestWorkingDirectory="C:\\nfsu2";
void logicalDrives(){ret(4,0);}
void driveType(){auto path=lower(guestString(arg(0)));ret(path.empty()||path.rfind("c:",0)==0?3:1,1);}
uint32_t fileError(){switch(errno){case ENOENT:return 2;case EACCES:case EPERM:return 5;case EEXIST:return 80;case ENOSPC:return 112;case EBADF:return 6;default:return 1;}}
// Guest paths resolved case-insensitively, keyed by the lower-case path inside the game folder. The game opens its
// stream archive (STREAML4RA.BUN) many times a second while driving; each resolution listed every directory on the
// way and canonicalised the result (~2% of the game thread in the VM, more on an SD card). Only paths whose every
// component exists are kept, so creating files cannot make an entry wrong; deleting, removing or moving clears it.
std::mutex resolvedGamePathsMutex;
std::unordered_map<std::string,std::filesystem::path> resolvedGamePaths;
void forgetResolvedGamePaths(){std::lock_guard<std::mutex> lock(resolvedGamePathsMutex);resolvedGamePaths.clear();}
// The guest passes names in its ANSI code page (Big5 on the Traditional Chinese game: a profile name typed with the
// on-screen keyboard over the default name is raw bytes, even half a character). The SD card's exFAT/FAT refuses
// names that are not valid UTF-8, which stopped the whole game when it created the profile folder. Bytes >= 0x80
// are kept on the card as %XX (upper-case hex) and turned back when the game lists a folder. Idempotent: the
// result holds no byte >= 0x80, so encoding a name read back from the card changes nothing.
std::string hostNameFromGuest(const std::string& name){
    std::string out;out.reserve(name.size());static const char hex[]="0123456789ABCDEF";
    for(unsigned char c:name){if(c<0x80)out.push_back(char(c));else{out.push_back('%');out.push_back(hex[c>>4]);out.push_back(hex[c&15]);}}
    return out;
}
std::string guestNameFromHost(const std::string& name){
    std::string out;out.reserve(name.size());
    auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<name.size();++i){
        if(name[i]=='%'&&i+2<name.size()+0&&digit(name[i+1])>=8&&digit(name[i+2])>=0){out.push_back(char(digit(name[i+1])*16+digit(name[i+2])));i+=2;}
        else out.push_back(name[i]);
    }
    return out;
}
std::filesystem::path gamePath(std::string guest){
    guest=hostNameFromGuest(guest);
    std::replace(guest.begin(),guest.end(),'\\','/');auto normalized=lower(guest);
    if(normalized=="c:/nfsu2")guest.clear();
    else if(normalized.rfind("c:/nfsu2/",0)==0)guest=guest.substr(9);
    else if(guest.find(':')!=std::string::npos||guest.rfind("//",0)==0)throw GuestStop("Unsupported guest path: "+guest);
    else {if(!guest.empty()&&guest.front()=='/')guest.erase(0,1);else{auto current=guestWorkingDirectory.substr(8);std::replace(current.begin(),current.end(),'\\','/');if(!current.empty())guest=current+"/"+guest;}}
    auto relative=std::filesystem::path(guest).lexically_normal();for(const auto& part:relative)if(part=="..")throw GuestStop("Guest path leaves nfsu2: "+guest);
    const std::string cacheKey=lower(relative.generic_string());
    {std::lock_guard<std::mutex> lock(resolvedGamePathsMutex);auto cached=resolvedGamePaths.find(cacheKey);if(cached!=resolvedGamePaths.end())return cached->second;}
    auto result=std::filesystem::path(hostGameRoot);bool everyPartExists=true;
    for(const auto& part:relative){if(part==".")continue;std::error_code error;bool found=false;
        for(auto& entry:std::filesystem::directory_iterator(result,error))if(lower(entry.path().filename().string())==lower(part.string())){result=entry.path();found=true;break;}
        if(!found){result/=part;everyPartExists=false;}
    }
    std::error_code canonicalError;auto root=std::filesystem::weakly_canonical(hostGameRoot,canonicalError);auto resolved=std::filesystem::weakly_canonical(result,canonicalError);if(canonicalError)resolved=result.lexically_normal();
    auto check=resolved.lexically_relative(root);for(const auto& part:check)if(part=="..")throw GuestStop("Guest symlink leaves data root");
    if(everyPartExists){std::lock_guard<std::mutex> lock(resolvedGamePathsMutex);resolvedGamePaths.emplace(cacheKey,resolved);}
    return resolved;
}
std::map<uint32_t,int> fileHandles;uint32_t nextFileHandle=0xf0060000;
// FMV (.vp6) files currently open: the intro-skip helper of the R36S host mashes the confirm keys while a movie
// plays (NFSU2_SKIP_INTRO). Counted per handle; the game opens each movie three times.
std::set<uint32_t> movieHandles;std::atomic<int> movieOpenCount{0};
extern "C" uint32_t getD3D9PresentCount();
// The game skips its CD-drive check when the game folder holds a file named
// "foobar" (its developer switch, 0x0057CAC0("foobar")). Copies made without
// it showed the insert-disc MessageBoxA. Present it as an empty file when absent.
bool discCheckSwitch(const std::filesystem::path& path){
    std::error_code error;
    return lower(path.filename().string())=="foobar"&&!std::filesystem::exists(path,error)&&
        std::filesystem::weakly_canonical(path.parent_path())==std::filesystem::weakly_canonical(hostGameRoot);
}
bool closeGuestFile(uint32_t handle){auto file=fileHandles.find(handle);if(file==fileHandles.end())return false;::close(file->second);fileHandles.erase(file);noteFileClose(handle);if(movieHandles.erase(handle)){movieOpenCount=int(movieHandles.size());NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest movie handle closed (%zu open) at frame %u",movieHandles.size(),getD3D9PresentCount());}return true;}
void fileType(){
    uint32_t handle=arg(0);
    if(handle>=0xf0000200u&&handle<=0xf0000208u){ret(2,1);return;}
    if(fileHandles.find(handle)!=fileHandles.end()){ret(1,1);return;}
    lastError=6;ret(0,1);
}
void longPath(){
    auto path=gamePath(guestString(arg(0)));std::error_code error;if(!std::filesystem::exists(path,error)){lastError=2;ret(0,3);return;}
    auto relative=path.lexically_relative(std::filesystem::weakly_canonical(hostGameRoot)).string();
    std::string result="C:\\nfsu2";if(relative!="."&&!relative.empty())result+="\\"+relative;std::replace(result.begin(),result.end(),'/','\\');
    if(arg(2)<=result.size()){ret(result.size()+1,3);return;}std::memcpy(ptr(arg(1),result.size()+1),result.c_str(),result.size()+1);ret(result.size(),3);
}
void createFile(){
    auto path=gamePath(guestString(arg(0)));uint32_t access=arg(1),disposition=arg(4),flags=arg(5);
    if(flags&0x40000000u)throw GuestStop("Overlapped CreateFile requires asynchronous file service");
    int options=access&0x40000000u?(access&0x80000000u?O_RDWR:O_WRONLY):O_RDONLY;
    switch(disposition){case 1:options|=O_CREAT|O_EXCL;break;case 2:options|=O_CREAT|O_TRUNC;break;case 3:break;case 4:options|=O_CREAT;break;case 5:options|=O_TRUNC;break;default:lastError=87;ret(UINT32_MAX,7);return;}
    std::error_code existsError;bool existed=std::filesystem::exists(path,existsError);
    // NFSU2_SKIP_MOVIES=ealogo,thx_logo,psa,fmvopening: those movies "do not exist" for the game (comma list of file
    // stems, case-insensitive), so it goes on to the next screen at once. Only read-opens of .vp6 files are refused.
    if(disposition==3&&!(access&0x40000000u)&&lower(path.extension().string())==".vp6"){
        static const std::string skipList=[]{const char* e=std::getenv("NFSU2_SKIP_MOVIES");return std::string(e?e:"");}();
        if(!skipList.empty()){
            const std::string stem=lower(path.stem().string());size_t pos=0;
            while(pos<=skipList.size()){size_t end=skipList.find(',',pos);if(end==std::string::npos)end=skipList.size();
                if(lower(skipList.substr(pos,end-pos))==stem){NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest movie %s skipped (NFSU2_SKIP_MOVIES)",path.filename().string().c_str());lastError=2;ret(UINT32_MAX,7);return;}
                pos=end+1;}
        }
    }
    int descriptor=disposition==3&&!(access&0x40000000u)&&discCheckSwitch(path) ? ::open("/dev/null",O_RDONLY|O_CLOEXEC) : ::open(path.c_str(),options|O_CLOEXEC,0600);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest file %s access=%x disposition=%u",path.filename().string().c_str(),access,disposition);
    noteFileOpen(path.filename().string());
    // A car folder missing from the game data: some regional installs lack CARS\CIVIC and CARS\RSX (others CORSA and
    // PEUGOT106), yet the game picks any of them as an AI or traffic car. The texture load (sub_0063A820) never gets
    // its callback when the file is missing, so the game sits on "Loading". Say so once per folder. Only CARS: the game
    // also probes folders that never exist (NFSUNDER\ZDIR.BIN at start).
    if(descriptor<0&&!(access&0x40000000u)&&errno==ENOENT){
        std::error_code folderError;const auto folder=path.parent_path();
        if(lower(folder.parent_path().filename().string())=="cars"&&!std::filesystem::exists(folder,folderError)){
            static std::set<std::string> reported;
            if(reported.size()<16&&reported.insert(lower(folder.filename().string())).second)
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","MISSING GAME DATA: gamedata/CARS/%s does not exist (wanted %s). The game hangs on \"Loading\" when it picks this car: copy the folder from a complete NFSU2 PC install (README, Complete car data).",
                    folder.filename().string().c_str(),path.filename().string().c_str());
        }
    }
    if(descriptor<0){if(access&0x40000000u)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest file %s could not be opened for writing: %s",path.string().c_str(),std::strerror(errno));lastError=fileError();ret(UINT32_MAX,7);return;}uint32_t handle=nextFileHandle++;fileHandles.emplace(handle,descriptor);if(lower(path.extension().string())==".vp6"){movieHandles.insert(handle);movieOpenCount=int(movieHandles.size());NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest movie %s opened (%zu open) at frame %u",path.filename().string().c_str(),movieHandles.size(),getD3D9PresentCount());}lastError=(disposition==2||disposition==4)&&existed?183:0;ret(handle,7);
}
void readFile(){
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,5);return;}if(arg(4))throw GuestStop("Overlapped ReadFile pending");
    if(arg(3))write32(arg(3),0);ssize_t count;
    const auto readStart=std::chrono::steady_clock::now();
    do{count=::read(file->second,ptr(arg(1),arg(2)),arg(2));}while(count<0&&errno==EINTR);
    noteFileRead(arg(0),arg(2),count);
    if(nfsIsGameThread()){nfsWaitAdd(10,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-readStart).count());if(count>0)nfsWaitAdd(11,uint64_t(count));}
    if(count<0){lastError=fileError();ret(0,5);return;}if(arg(3))write32(arg(3),count);ret(1,5);
}
void writeFile(){
    if(arg(4))throw GuestStop("Overlapped WriteFile pending");
    if(!arg(3)||!memory->writable(arg(3),4)){lastError=87;ret(0,5);return;}write32(arg(3),0);
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,5);return;}
    if(!memory->readable(arg(1),arg(2))){lastError=1784;ret(0,5);return;}
    ssize_t count;do{count=::write(file->second,ptr(arg(1),arg(2)),arg(2));}while(count<0&&errno==EINTR);
    if(count<0){NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest WriteFile failed: %s",std::strerror(errno));lastError=fileError();ret(0,5);return;}write32(arg(3),uint32_t(count));ret(1,5);
}
void flushFileBuffers(){auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,1);return;}
    int result;do{result=::fsync(file->second);}while(result&&errno==EINTR);if(result){lastError=fileError();ret(0,1);return;}ret(1,1);
}
void setEndOfFile(){auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(0,1);return;}
    auto position=::lseek(file->second,0,SEEK_CUR);if(position<0||::ftruncate(file->second,position)){lastError=fileError();ret(0,1);return;}ret(1,1);
}
void fileSize(){auto file=fileHandles.find(arg(0));struct stat status{};if(file==fileHandles.end()){lastError=6;ret(UINT32_MAX,2);return;}if(fstat(file->second,&status)){lastError=fileError();ret(UINT32_MAX,2);return;}uint64_t size=status.st_size;if(arg(1))write32(arg(1),size>>32);lastError=0;ret(size,2);}
void fileAttributes(){auto path=gamePath(guestString(arg(0)));struct stat status{};if(discCheckSwitch(path)){ret(0x20,1);return;}if(stat(path.c_str(),&status)){lastError=fileError();ret(UINT32_MAX,1);return;}ret(S_ISDIR(status.st_mode)?0x10:0x20,1);}
void setFilePointer(){
    auto file=fileHandles.find(arg(0));if(file==fileHandles.end()){lastError=6;ret(UINT32_MAX,4);return;}
    uint32_t method=arg(3),high=arg(2);if(method>2){lastError=87;ret(UINT32_MAX,4);return;}
    int64_t distance=int32_t(arg(1));if(high){uint64_t bits=(uint64_t(read32(high))<<32)|arg(1);std::memcpy(&distance,&bits,8);}
    off_t result=::lseek(file->second,off_t(distance),method==0?SEEK_SET:method==1?SEEK_CUR:SEEK_END);
    if(result<0){lastError=errno==EINVAL?131:fileError();ret(UINT32_MAX,4);return;}
    if(!high&&uint64_t(result)>UINT32_MAX){lastError=87;ret(UINT32_MAX,4);return;}
    if(high)write32(high,uint64_t(result)>>32);lastError=0;ret(uint32_t(result),4);
}
// Keep Windows profile folders alongside the user's game data on Android.
void shellFolderPath(){
    uint32_t folder=arg(1),out=arg(4);if(!out||!memory->writable(out,260)){ret(0x80070057u,5);return;}
    *static_cast<char*>(ptr(out,1))=0;
    if(arg(2)||arg(3)>1||(folder&~0xc0ffu)){ret(0x80070057u,5);return;}
    std::string suffix;switch(folder&0xff){case 0x1c:suffix="AppData\\Local";break;case 0x1a:suffix="AppData\\Roaming";break;case 5:suffix="Documents";break;default:ret(0x80070057u,5);return;}
    std::string logical="C:\\nfsu2\\"+suffix;auto path=gamePath(logical);std::error_code error;
    if(folder&0x8000)std::filesystem::create_directories(path,error);
    if(error){ret(0x80070005u,5);return;}
    if(!(folder&0x4000)&&!std::filesystem::is_directory(path,error)){ret(0x80070003u,5);return;}
    std::memcpy(ptr(out,logical.size()+1),logical.c_str(),logical.size()+1);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest profile folder %s",logical.c_str());ret(0,5);
}
struct GuestFileSearch {std::vector<std::filesystem::path> paths;size_t next=0;};
std::map<uint32_t,GuestFileSearch> fileSearches;uint32_t nextSearchHandle=0xf0070000;
bool filePatternMatches(std::string pattern,std::string name){
    pattern=lower(pattern);name=lower(name);if(pattern=="*.*")pattern="*";
    size_t p=0,n=0,star=std::string::npos,retry=0;
    while(n<name.size()){
        if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==name[n])){++p;++n;}
        else if(p<pattern.size()&&pattern[p]=='*'){star=p++;retry=n;}
        else if(star!=std::string::npos){p=star+1;n=++retry;}else return false;
    }while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();
}
bool writeFindData(uint32_t out,const std::filesystem::path& path){
    if(!out||!memory->writable(out,320)){lastError=87;return false;}
    struct stat status{};if(::stat(path.c_str(),&status)){lastError=fileError();return false;}
    auto name=guestNameFromHost(path.filename().string());if(name.size()>=260){lastError=206;return false;}
    std::memset(ptr(out,320),0,320);write32(out,S_ISDIR(status.st_mode)?0x10:0x20);
    auto time=[&](uint32_t offset,int64_t seconds){uint64_t ticks=(seconds+11644473600ll)*10000000ull;std::memcpy(ptr(out+offset,8),&ticks,8);};
    time(4,status.st_ctime);time(12,status.st_atime);time(20,status.st_mtime);
    uint64_t size=S_ISDIR(status.st_mode)?0:uint64_t(status.st_size);write32(out+28,size>>32);write32(out+32,uint32_t(size));
    std::memcpy(ptr(out+44,name.size()+1),name.c_str(),name.size()+1);return true;
}
void findFirstFile(){
    auto requested=guestString(arg(0));std::replace(requested.begin(),requested.end(),'\\','/');
    if(requested.empty()||requested.back()=='/'){lastError=123;ret(UINT32_MAX,2);return;}
    auto separator=requested.find_last_of('/');auto pattern=hostNameFromGuest(requested.substr(separator==std::string::npos?0:separator+1));
    auto directory=gamePath(separator==std::string::npos?".":requested.substr(0,separator));
    std::error_code error;GuestFileSearch search;
    if(std::filesystem::is_directory(directory,error))for(const char* dot:{".",".."})if(filePatternMatches(pattern,dot))search.paths.push_back(directory/dot);
    for(auto& entry:std::filesystem::directory_iterator(directory,error)){
        if(!filePatternMatches(pattern,entry.path().filename().string()))continue;
        // Apply the same containment check used for ordinary file opens.
        auto logical="C:/nfsu2/"+entry.path().lexically_relative(std::filesystem::weakly_canonical(hostGameRoot)).string();
        gamePath(logical);search.paths.push_back(entry.path());
    }
    if(error){lastError=error==std::errc::no_such_file_or_directory?3:5;ret(UINT32_MAX,2);return;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest FindFirstFile %s -> %zu entries",requested.c_str(),search.paths.size());
    if(search.paths.empty()){lastError=2;ret(UINT32_MAX,2);return;}
    std::sort(search.paths.begin(),search.paths.end());
    if(!writeFindData(arg(1),search.paths.front())){ret(UINT32_MAX,2);return;}
    search.next=1;uint32_t handle=nextSearchHandle++;fileSearches.emplace(handle,std::move(search));ret(handle,2);
}
void findNextFile(){auto found=fileSearches.find(arg(0));if(found==fileSearches.end()){lastError=6;ret(0,2);return;}
    auto& search=found->second;if(search.next==search.paths.size()){lastError=18;ret(0,2);return;}
    if(!writeFindData(arg(1),search.paths[search.next])){ret(0,2);return;}++search.next;ret(1,2);
}
void findClose(){if(!fileSearches.erase(arg(0))){lastError=6;ret(0,1);return;}ret(1,1);}
void createDirectory(){if(arg(1))throw GuestStop("CreateDirectory security attributes pending");auto path=gamePath(guestString(arg(0)));
    const int made=::mkdir(path.c_str(),0700);NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest CreateDirectory %s -> %s",path.string().c_str(),made?std::strerror(errno):"ok");
    if(made){lastError=errno==EEXIST?183:fileError();ret(0,2);return;}ret(1,2);
}
void fullPathName(){
    auto requested=guestString(arg(0));if(requested.empty()){lastError=123;ret(0,4);return;}
    auto path=gamePath(requested);auto relative=path.lexically_relative(std::filesystem::weakly_canonical(hostGameRoot)).string();
    std::string logical="C:\\nfsu2";if(relative!="."&&!relative.empty())logical+="\\"+relative;
    std::replace(logical.begin(),logical.end(),'/','\\');if(requested.back()=='/'||requested.back()=='\\')logical+='\\';
    uint32_t capacity=arg(1),out=arg(2),part=arg(3);if(capacity<=logical.size()){ret(uint32_t(logical.size()+1),4);return;}
    if(!out||!memory->writable(out,logical.size()+1)||(part&&!memory->writable(part,4))){lastError=87;ret(0,4);return;}
    std::memcpy(ptr(out,logical.size()+1),logical.c_str(),logical.size()+1);
    if(part)write32(part,logical.back()=='\\'?0:out+uint32_t(logical.find_last_of('\\')+1));ret(uint32_t(logical.size()),4);
}
// DeleteFileA / MoveFileA inside the game folder (gamePath refuses anything outside it).
void deleteFile(){
    auto path=gamePath(guestString(arg(0)));
    const int gone=::unlink(path.c_str());NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest DeleteFile %s -> %s",path.string().c_str(),gone?std::strerror(errno):"ok");
    if(gone){lastError=fileError();ret(0,1);return;}
    forgetResolvedGamePaths();lastError=0;ret(1,1);
}
void removeDirectory(){
    auto path=gamePath(guestString(arg(0)));
    if(::rmdir(path.c_str())){lastError=errno==ENOTEMPTY||errno==EEXIST?145:fileError();ret(0,1);return;}  // ERROR_DIR_NOT_EMPTY
    forgetResolvedGamePaths();lastError=0;ret(1,1);
}
void moveFile(){
    auto from=gamePath(guestString(arg(0))),to=gamePath(guestString(arg(1)));
    struct stat status{};if(!::stat(to.c_str(),&status)){lastError=183;ret(0,2);return;}  // ERROR_ALREADY_EXISTS
    const int moved=::rename(from.c_str(),to.c_str());NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest MoveFile %s -> %s: %s",from.string().c_str(),to.string().c_str(),moved?std::strerror(errno):"ok");
    if(moved){lastError=fileError();ret(0,2);return;}
    forgetResolvedGamePaths();lastError=0;ret(1,2);
}
// GetFileTime(handle, creation, access, write): FILETIME is 100 ns ticks since 1601.
void fileTime(){
    auto file=fileHandles.find(arg(0));struct stat status{};
    if(file==fileHandles.end()){lastError=6;ret(0,4);return;}
    if(fstat(file->second,&status)){lastError=fileError();ret(0,4);return;}
    auto ticks=[](const timespec& time){return uint64_t(time.tv_sec)*10000000ull+uint64_t(time.tv_nsec)/100u+116444736000000000ull;};
    uint64_t values[3]={ticks(status.st_mtim),ticks(status.st_atim),ticks(status.st_mtim)};  // no creation time: use the last write
    for(unsigned i=0;i<3;++i)if(arg(1+i))std::memcpy(ptr(arg(1+i),8),&values[i],8);
    lastError=0;ret(1,4);
}
const std::map<std::string,recomp_func_t> fileFunctions={
    {"GetFileTime",fileTime},{"DeleteFileA",deleteFile},{"RemoveDirectoryA",removeDirectory},{"MoveFileA",moveFile},
    {"GetFullPathNameA",fullPathName},{"WriteFile",writeFile},{"FlushFileBuffers",flushFileBuffers},{"SetEndOfFile",setEndOfFile},
    {"FindFirstFileA",findFirstFile},{"FindNextFileA",findNextFile},{"FindClose",findClose},{"CreateDirectoryA",createDirectory},
    {"SetFilePointer",setFilePointer},
    {"GetLogicalDrives",logicalDrives},{"GetDriveTypeA",driveType},
    {"GetLongPathNameA",longPath},{"CreateFileA",createFile},{"ReadFile",readFile},{"GetFileSize",fileSize},{"GetFileAttributesA",fileAttributes}
};
