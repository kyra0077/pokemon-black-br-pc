// Platform layer required by the melonDS core, trimmed down for a
// single-game, single-instance build (no Wi-Fi, local multiplayer,
// camera or microphone).

#include <cstdarg>
#include <cstdio>
#include <string>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>

#include <SDL.h>

#include "Platform.h"
#include "SPI_Firmware.h"
#include "App.h"

namespace melonDS::Platform
{

static std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void SignalStop(StopReason reason, void* userdata)
{
    if (userdata) ((App*)userdata)->OnEmuStop(reason);
}

static bool Exists(const std::string& path)
{
    return GetFileAttributesW(Widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring ModeString(FileMode mode, bool file_exists)
{
    std::wstring m;
    if (mode & FileMode::Append) m = L"a";
    else if (!(mode & FileMode::Write)) m = L"r";
    else if (mode & FileMode::NoCreate) m = L"r";
    else if ((mode & FileMode::Preserve) && file_exists) m = L"r";
    else m = L"w";

    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite) m += L"+";
    if (!(mode & FileMode::Text)) m += L"b";
    return m;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;

    bool exists = Exists(path);
    if ((mode & FileMode::NoCreate) && !exists)
        return nullptr;

    FILE* f = _wfopen(Widen(path).c_str(), ModeString(mode, exists).c_str());
    return reinterpret_cast<FileHandle*>(f);
}

std::string GetLocalFilePath(const std::string& filename)
{
    if (filename.size() > 1 && (filename[1] == ':' || filename[0] == '\\' || filename[0] == '/'))
        return filename;
    return App::ExeDirectory() + filename;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool CloseFile(FileHandle* file) { return fclose((FILE*)file) == 0; }
bool IsEndOfFile(FileHandle* file) { return feof((FILE*)file) != 0; }
bool FileReadLine(char* str, int count, FileHandle* file) { return fgets(str, count, (FILE*)file) != nullptr; }
bool FileExists(const std::string& name) { return Exists(name); }
bool LocalFileExists(const std::string& name) { return Exists(GetLocalFilePath(name)); }

bool CheckFileWritable(const std::string& filepath)
{
    if (Exists(filepath))
    {
        FileHandle* f = OpenFile(filepath, FileMode::Append);
        if (!f) return false;
        CloseFile(f);
        return true;
    }
    FileHandle* f = OpenFile(filepath, FileMode::Write);
    if (!f) return false;
    CloseFile(f);
    DeleteFileW(Widen(filepath).c_str());
    return true;
}

bool CheckLocalFileWritable(const std::string& name) { return CheckFileWritable(GetLocalFilePath(name)); }

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int o = SEEK_SET;
    if (origin == FileSeekOrigin::Current) o = SEEK_CUR;
    else if (origin == FileSeekOrigin::End) o = SEEK_END;
    return _fseeki64((FILE*)file, offset, o) == 0;
}

void FileRewind(FileHandle* file) { rewind((FILE*)file); }
u64 FilePosition(FileHandle* file) { return _ftelli64((FILE*)file); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) { return fread(data, size, count, (FILE*)file); }
bool FileFlush(FileHandle* file) { return fflush((FILE*)file) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) { return fwrite(data, size, count, (FILE*)file); }

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    if (!fmt) return 0;
    va_list args;
    va_start(args, fmt);
    u64 ret = vfprintf((FILE*)file, fmt, args);
    va_end(args);
    return ret;
}

u64 FileLength(FileHandle* file)
{
    FILE* f = (FILE*)file;
    s64 pos = _ftelli64(f);
    _fseeki64(f, 0, SEEK_END);
    s64 len = _ftelli64(f);
    _fseeki64(f, pos, SEEK_SET);
    return len;
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    App::LogV((int)level, fmt, args);
    va_end(args);
}

struct ThreadBox { std::function<void()> func; SDL_Thread* thread; };

static int ThreadEntry(void* data)
{
    ((ThreadBox*)data)->func();
    return 0;
}

Thread* Thread_Create(std::function<void()> func)
{
    auto* box = new ThreadBox{std::move(func), nullptr};
    box->thread = SDL_CreateThread(ThreadEntry, "melonDS worker", box);
    return (Thread*)box;
}

void Thread_Wait(Thread* thread)
{
    auto* box = (ThreadBox*)thread;
    if (box->thread)
    {
        SDL_WaitThread(box->thread, nullptr);
        box->thread = nullptr;
    }
}

void Thread_Free(Thread* thread)
{
    Thread_Wait(thread);
    delete (ThreadBox*)thread;
}

Semaphore* Semaphore_Create() { return (Semaphore*)SDL_CreateSemaphore(0); }
void Semaphore_Free(Semaphore* sema) { SDL_DestroySemaphore((SDL_sem*)sema); }
void Semaphore_Reset(Semaphore* sema) { while (SDL_SemTryWait((SDL_sem*)sema) == 0) {} }
void Semaphore_Wait(Semaphore* sema) { SDL_SemWait((SDL_sem*)sema); }

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    if (!timeout_ms) return SDL_SemTryWait((SDL_sem*)sema) == 0;
    return SDL_SemWaitTimeout((SDL_sem*)sema, timeout_ms) == 0;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    for (int i = 0; i < count; i++) SDL_SemPost((SDL_sem*)sema);
}

Mutex* Mutex_Create() { return (Mutex*)SDL_CreateMutex(); }
void Mutex_Free(Mutex* mutex) { SDL_DestroyMutex((SDL_mutex*)mutex); }
void Mutex_Lock(Mutex* mutex) { SDL_LockMutex((SDL_mutex*)mutex); }
void Mutex_Unlock(Mutex* mutex) { SDL_UnlockMutex((SDL_mutex*)mutex); }
bool Mutex_TryLock(Mutex* mutex) { return SDL_TryLockMutex((SDL_mutex*)mutex) == 0; }

void Sleep(u64 usecs) { SDL_Delay((Uint32)(usecs / 1000)); }

u64 GetMSCount() { return SDL_GetTicks64(); }

u64 GetUSCount()
{
    static const u64 freq = SDL_GetPerformanceFrequency();
    return (u64)((double)SDL_GetPerformanceCounter() * 1000000.0 / (double)freq);
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    if (userdata) ((App*)userdata)->OnSaveWrite(savedata, savelen);
}

void WriteGBASave(const u8*, u32, u32, u32, void*) {}

// The generated firmware lives in memory only; nothing to persist.
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}

void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*)
{
    for (int i = 0; i < width * height; i++) frame[i] = 0;
}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }

// AAC is only used by DSi-mode DSP software.
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char* lib) { return (DynamicLibrary*)SDL_LoadObject(lib); }
void DynamicLibrary_Unload(DynamicLibrary* lib) { SDL_UnloadObject(lib); }
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) { return SDL_LoadFunction(lib, name); }

}
