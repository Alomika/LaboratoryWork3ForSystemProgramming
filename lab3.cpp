#include <windows.h>
#include <algorithm>
#include <climits>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

#define MAX_QUEUE 100000
#define MAX_FILES 4096
#define MAX_PATH_LEN 260
#define MAX_WORKERS 256

struct QueueItem {
    int fileIndex;
};

struct SharedData {
    QueueItem queue[MAX_QUEUE];
    int head;
    int tail;
    int count;
    bool done;

    int totalFiles;
    char filePaths[MAX_FILES][MAX_PATH_LEN];
    long long fileMinPrime[MAX_FILES];
    long long fileMaxPrime[MAX_FILES];

    long long globalMin;
    long long globalMax;

    bool workerStopRequested[MAX_WORKERS];
};

struct WorkerEntry {
    int id;
    HANDLE process;
    bool stopRequested;
};

struct ControlContext {
    HANDLE hMutex;
    HANDLE hSem;
    SharedData* data;
    vector<WorkerEntry>* workers;
    CRITICAL_SECTION* workersCs;
    bool* stopController;
    string mapName;
    string mutexName;
    string semName;
    int* totalSpawned;
};

class SafeQueue {
public:
    SafeQueue(HANDLE m, HANDLE s, SharedData* d)
        : hMutex(m), hSem(s), data(d) {}

    bool push(const QueueItem& item)
    {
        WaitForSingleObject(hMutex, INFINITE);

        if (data->count >= MAX_QUEUE) {
            ReleaseMutex(hMutex);
            return false;
        }

        data->queue[data->tail] = item;
        data->tail = (data->tail + 1) % MAX_QUEUE;
        data->count++;

        ReleaseMutex(hMutex);
        ReleaseSemaphore(hSem, 1, NULL);
        return true;
    }

    bool popForWorker(QueueItem& out, int workerId)
    {
        for (;;) {
            WaitForSingleObject(hSem, INFINITE);
            WaitForSingleObject(hMutex, INFINITE);

            const bool stopRequested =
                (workerId >= 0 && workerId < MAX_WORKERS) ? data->workerStopRequested[workerId] : false;

            if (stopRequested) {
                if (data->count > 0) {
                    // Do not lose a real task signal when this worker exits.
                    ReleaseSemaphore(hSem, 1, NULL);
                }
                ReleaseMutex(hMutex);
                return false;
            }

            if (data->count > 0) {
                out = data->queue[data->head];
                data->head = (data->head + 1) % MAX_QUEUE;
                data->count--;
                ReleaseMutex(hMutex);
                return true;
            }

            if (data->done) {
                ReleaseMutex(hMutex);
                return false;
            }

            ReleaseMutex(hMutex);
        }
    }

private:
    HANDLE hMutex;
    HANDLE hSem;
    SharedData* data;
};

bool isPrime(long long n)
{
    if (n < 2) return false;
    if ((n % 2) == 0) return n == 2;

    for (long long i = 3; i <= (n / i); i += 2) {
        if ((n % i) == 0)
            return false;
    }
    return true;
}

bool endsWithTxt(const string& name)
{
    if (name.size() < 4) return false;
    const size_t n = name.size();
    return tolower((unsigned char)name[n - 4]) == '.' &&
           tolower((unsigned char)name[n - 3]) == 't' &&
           tolower((unsigned char)name[n - 2]) == 'x' &&
           tolower((unsigned char)name[n - 1]) == 't';
}

string fileNameOnly(const string& path)
{
    const size_t pos = path.find_last_of("\\/");
    return (pos == string::npos) ? path : path.substr(pos + 1);
}

void collectTxtFilesRecursive(const string& dir, vector<string>& outFiles)
{
    string pattern = dir;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
        pattern += "\\";
    pattern += "*";

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        const string name = fd.cFileName;
        if (name == "." || name == "..")
            continue;

        string fullPath = dir;
        if (!fullPath.empty() && fullPath.back() != '\\' && fullPath.back() != '/')
            fullPath += "\\";
        fullPath += name;

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            collectTxtFilesRecursive(fullPath, outFiles);
        } else if (endsWithTxt(name)) {
            outFiles.push_back(fullPath);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

bool processOneFile(const char* filePath, long long& minPrime, long long& maxPrime)
{
    HANDLE hFile = CreateFileA(
        filePath,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    minPrime = LLONG_MAX;
    maxPrime = LLONG_MIN;

    long long value = 0;
    bool readingNumber = false;

    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
        for (DWORD i = 0; i < bytesRead; ++i) {
            const unsigned char c = (unsigned char)buf[i];
            if (isdigit(c)) {
                readingNumber = true;
                const int digit = c - '0';

                if (value > (LLONG_MAX - digit) / 10) {
                    // Clamp on overflow; still treat it as a completed huge number.
                    value = LLONG_MAX;
                } else {
                    value = value * 10 + digit;
                }
            } else if (readingNumber) {
                if (isPrime(value)) {
                    if (value < minPrime) minPrime = value;
                    if (value > maxPrime) maxPrime = value;
                }
                value = 0;
                readingNumber = false;
            }
        }
    }

    CloseHandle(hFile);

    if (readingNumber && isPrime(value)) {
        if (value < minPrime) minPrime = value;
        if (value > maxPrime) maxPrime = value;
    }

    return true;
}

int runWorker(const string& mapName, const string& mutexName, const string& semName, int workerId)
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
    HANDLE hMutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
    HANDLE hSem = OpenSemaphoreA(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, semName.c_str());

    if (!hMap || !hMutex || !hSem) {
        cout << "Worker failed to open shared objects. GetLastError=" << GetLastError() << "\n";
        if (hMap) CloseHandle(hMap);
        if (hMutex) CloseHandle(hMutex);
        if (hSem) CloseHandle(hSem);
        return 1;
    }

    SharedData* data = (SharedData*)MapViewOfFile(
        hMap,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0,
        0,
        sizeof(SharedData));

    if (!data) {
        cout << "Worker failed to map shared memory. GetLastError=" << GetLastError() << "\n";
        CloseHandle(hMap);
        CloseHandle(hMutex);
        CloseHandle(hSem);
        return 1;
    }

    SafeQueue queue(hMutex, hSem, data);

    QueueItem item;
    while (queue.popForWorker(item, workerId)) {
        if (item.fileIndex < 0 || item.fileIndex >= data->totalFiles)
            continue;

        long long localMin = LLONG_MAX;
        long long localMax = LLONG_MIN;
        const bool ok = processOneFile(data->filePaths[item.fileIndex], localMin, localMax);

        WaitForSingleObject(hMutex, INFINITE);
        if (!ok) {
            data->fileMinPrime[item.fileIndex] = LLONG_MAX;
            data->fileMaxPrime[item.fileIndex] = LLONG_MIN;
        } else {
            data->fileMinPrime[item.fileIndex] = localMin;
            data->fileMaxPrime[item.fileIndex] = localMax;

            if (localMin != LLONG_MAX && localMin < data->globalMin)
                data->globalMin = localMin;
            if (localMax != LLONG_MIN && localMax > data->globalMax)
                data->globalMax = localMax;
        }
        ReleaseMutex(hMutex);
    }

    UnmapViewOfFile(data);
    CloseHandle(hMap);
    CloseHandle(hMutex);
    CloseHandle(hSem);
    return 0;
}

bool makeWide(const string& s, wstring& out)
{
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (needed <= 0) return false;
    vector<wchar_t> buf((size_t)needed);
    if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), needed) <= 0)
        return false;
    out.assign(buf.data());
    return true;
}

HANDLE spawnWorker(int workerId, const string& mapName, const string& mutexName, const string& semName)
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return NULL;
    }

    wstring mapW;
    wstring mutexW;
    wstring semW;
    if (!makeWide(mapName, mapW) || !makeWide(mutexName, mutexW) || !makeWide(semName, semW)) {
        return NULL;
    }

    wchar_t idBuf[32] = {};
    _snwprintf(idBuf, 31, L"%d", workerId);

    wstring cmd =
        L"\"" + wstring(exePath) +
        L"\" --worker " +
        wstring(idBuf) +
        L" \"" + mapW +
        L"\" \"" + mutexW +
        L"\" \"" + semW +
        L"\"";

    vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(
            NULL,
            mutableCmd.data(),
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &si,
            &pi)) {
        return NULL;
    }

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

void reapFinishedWorkers(vector<WorkerEntry>& workers)
{
    for (size_t i = 0; i < workers.size(); ++i) {
        if (!workers[i].process)
            continue;

        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(workers[i].process, &code) && code != STILL_ACTIVE) {
            CloseHandle(workers[i].process);
            workers[i].process = NULL;
        }
    }

    workers.erase(
        remove_if(workers.begin(), workers.end(), [](const WorkerEntry& w) { return w.process == NULL; }),
        workers.end());
}

int countActiveWorkers(const vector<WorkerEntry>& workers)
{
    int count = 0;
    for (size_t i = 0; i < workers.size(); ++i) {
        if (workers[i].process != NULL)
            ++count;
    }
    return count;
}

DWORD WINAPI controlThreadProc(LPVOID param)
{
    ControlContext* ctx = (ControlContext*)param;
    HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);

    cout << "\nControls: '+' add worker, '-' remove worker, 'q' stop control thread (no Enter needed)\n";

    while (!(*ctx->stopController)) {
        if (hInput == INVALID_HANDLE_VALUE || hInput == NULL) {
            Sleep(50);
            continue;
        }

        DWORD numEvents = 0;
        if (!GetNumberOfConsoleInputEvents(hInput, &numEvents) || numEvents == 0) {
            Sleep(25);
            continue;
        }

        INPUT_RECORD record;
        DWORD eventsRead = 0;
        if (!ReadConsoleInput(hInput, &record, 1, &eventsRead) || eventsRead == 0) {
            continue;
        }

        if (record.EventType != KEY_EVENT) {
            continue;
        }

        if (!record.Event.KeyEvent.bKeyDown) {
            continue;
        }

        const char cmd = record.Event.KeyEvent.uChar.AsciiChar;

        if (cmd == '+') {
            EnterCriticalSection(ctx->workersCs);
            reapFinishedWorkers(*ctx->workers);

            bool used[MAX_WORKERS] = {};
            for (size_t i = 0; i < ctx->workers->size(); ++i) {
                if ((*ctx->workers)[i].process != NULL && (*ctx->workers)[i].id >= 0 && (*ctx->workers)[i].id < MAX_WORKERS) {
                    used[(*ctx->workers)[i].id] = true;
                }
            }

            int freeId = -1;
            for (int i = 0; i < MAX_WORKERS; ++i) {
                if (!used[i]) {
                    freeId = i;
                    break;
                }
            }
            LeaveCriticalSection(ctx->workersCs);

            if (freeId < 0) {
                cout << "Cannot add worker: max reached\n";
                continue;
            }

            WaitForSingleObject(ctx->hMutex, INFINITE);
            ctx->data->workerStopRequested[freeId] = false;
            ReleaseMutex(ctx->hMutex);

            HANDLE hProc = spawnWorker(freeId, ctx->mapName, ctx->mutexName, ctx->semName);
            if (!hProc) {
                cout << "Failed to create new worker\n";
                continue;
            }

            EnterCriticalSection(ctx->workersCs);
            ctx->workers->push_back({ freeId, hProc, false });
            if (ctx->totalSpawned) {
                ++(*ctx->totalSpawned);
            }
            const int active = countActiveWorkers(*ctx->workers);
            LeaveCriticalSection(ctx->workersCs);

            cout << "Added worker " << freeId << ". Active workers: " << active << "\n";
        } else if (cmd == '-') {
            int selectedId = -1;

            EnterCriticalSection(ctx->workersCs);
            reapFinishedWorkers(*ctx->workers);

            const int active = countActiveWorkers(*ctx->workers);
            if (active <= 1) {
                LeaveCriticalSection(ctx->workersCs);
                cout << "Cannot remove last active worker\n";
                continue;
            }

            for (int i = (int)ctx->workers->size() - 1; i >= 0; --i) {
                if ((*ctx->workers)[i].process != NULL && !(*ctx->workers)[i].stopRequested) {
                    (*ctx->workers)[i].stopRequested = true;
                    selectedId = (*ctx->workers)[i].id;
                    break;
                }
            }
            LeaveCriticalSection(ctx->workersCs);

            if (selectedId < 0) {
                cout << "No worker available for graceful removal\n";
                continue;
            }

            WaitForSingleObject(ctx->hMutex, INFINITE);
            ctx->data->workerStopRequested[selectedId] = true;
            ReleaseMutex(ctx->hMutex);

            // Wake an idle worker so it can see stop flag and exit immediately.
            ReleaseSemaphore(ctx->hSem, 1, NULL);

            cout << "Requested stop for worker " << selectedId << "\n";
        } else if (cmd == 'q' || cmd == 'Q') {
            cout << "Control thread stopping on user request\n";
            break;
        }
    }

    return 0;
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && string(argv[1]) == "--worker") {
        if (argc < 6) {
            cout << "Worker arguments missing\n";
            return 1;
        }

        const int workerId = atoi(argv[2]);
        if (workerId < 0 || workerId >= MAX_WORKERS) {
            cout << "Invalid worker id\n";
            return 1;
        }

        return runWorker(argv[3], argv[4], argv[5], workerId);
    }

    string inputDir = ".";
    int initialWorkers = 0;

    if (argc >= 2)
        inputDir = argv[1];
    if (argc >= 3)
        initialWorkers = atoi(argv[2]);

    if (initialWorkers <= 0) {
        cout << "Enter initial worker count (1-" << MAX_WORKERS << "): ";
        cin >> initialWorkers;
    }

    if (initialWorkers <= 0 || initialWorkers > MAX_WORKERS) {
        cout << "Invalid worker count\n";
        return 1;
    }

    const DWORD attr = GetFileAttributesA(inputDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        cout << "Directory not found: " << inputDir << "\n";
        return 1;
    }

    vector<string> files;
    collectTxtFilesRecursive(inputDir, files);

    if (files.empty()) {
        cout << "No .txt files found in: " << inputDir << "\n";
        return 1;
    }

    if (files.size() > MAX_FILES) {
        cout << "Too many files (max " << MAX_FILES << ")\n";
        return 1;
    }

    const DWORD pid = GetCurrentProcessId();
    char mapNameBuf[128] = {};
    char mutexNameBuf[128] = {};
    char semNameBuf[128] = {};
    _snprintf(mapNameBuf, sizeof(mapNameBuf) - 1, "Local\\Lab3Map_%lu", (unsigned long)pid);
    _snprintf(mutexNameBuf, sizeof(mutexNameBuf) - 1, "Local\\Lab3Mutex_%lu", (unsigned long)pid);
    _snprintf(semNameBuf, sizeof(semNameBuf) - 1, "Local\\Lab3Sem_%lu", (unsigned long)pid);

    const string mapName = mapNameBuf;
    const string mutexName = mutexNameBuf;
    const string semName = semNameBuf;

    HANDLE hMap = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SharedData),
        mapName.c_str());

    if (!hMap) {
        cout << "CreateFileMapping failed. GetLastError=" << GetLastError() << "\n";
        return 1;
    }

    SharedData* data = (SharedData*)MapViewOfFile(
        hMap,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0,
        0,
        sizeof(SharedData));

    if (!data) {
        cout << "MapViewOfFile failed. GetLastError=" << GetLastError() << "\n";
        CloseHandle(hMap);
        return 1;
    }

    HANDLE hMutex = CreateMutexA(NULL, FALSE, mutexName.c_str());
    HANDLE hSem = CreateSemaphoreA(NULL, 0, MAX_QUEUE, semName.c_str());

    if (!hMutex || !hSem) {
        cout << "Failed to create synchronization objects. GetLastError=" << GetLastError() << "\n";
        if (hMutex) CloseHandle(hMutex);
        if (hSem) CloseHandle(hSem);
        UnmapViewOfFile(data);
        CloseHandle(hMap);
        return 1;
    }

    ZeroMemory(data, sizeof(SharedData));
    data->head = 0;
    data->tail = 0;
    data->count = 0;
    data->done = false;
    data->totalFiles = (int)files.size();
    data->globalMin = LLONG_MAX;
    data->globalMax = LLONG_MIN;

    for (int i = 0; i < data->totalFiles; ++i) {
        ZeroMemory(data->filePaths[i], MAX_PATH_LEN);
        strncpy(data->filePaths[i], files[i].c_str(), MAX_PATH_LEN - 1);
        data->filePaths[i][MAX_PATH_LEN - 1] = '\0';
        data->fileMinPrime[i] = LLONG_MAX;
        data->fileMaxPrime[i] = LLONG_MIN;
    }

    SafeQueue queue(hMutex, hSem, data);

    vector<WorkerEntry> workers;
    workers.reserve(MAX_WORKERS);

    CRITICAL_SECTION workersCs;
    InitializeCriticalSection(&workersCs);

    cout << "Found " << files.size() << " .txt files\n";
    cout << "Starting with " << initialWorkers << " worker processes\n";

    for (int i = 0; i < initialWorkers; ++i) {
        HANDLE hProc = spawnWorker(i, mapName, mutexName, semName);
        if (!hProc) {
            cout << "Failed to start worker " << i << "\n";
            DeleteCriticalSection(&workersCs);
            UnmapViewOfFile(data);
            CloseHandle(hMap);
            CloseHandle(hMutex);
            CloseHandle(hSem);
            return 1;
        }
        workers.push_back({ i, hProc, false });
    }

    int totalSpawnedWorkers = initialWorkers;

    bool stopController = false;
    ControlContext ctx = {};
    ctx.hMutex = hMutex;
    ctx.hSem = hSem;
    ctx.data = data;
    ctx.workers = &workers;
    ctx.workersCs = &workersCs;
    ctx.stopController = &stopController;
    ctx.mapName = mapName;
    ctx.mutexName = mutexName;
    ctx.semName = semName;
    ctx.totalSpawned = &totalSpawnedWorkers;

    HANDLE hControlThread = CreateThread(NULL, 0, controlThreadProc, &ctx, 0, NULL);
    if (!hControlThread) {
        cout << "Failed to start control thread\n";
        stopController = true;
    }

    const int batchSize = 16;
    int nextFile = 0;

    while (nextFile < data->totalFiles) {
        int pushed = 0;
        while (pushed < batchSize && nextFile < data->totalFiles) {
            if (queue.push({ nextFile })) {
                ++nextFile;
                ++pushed;
            } else {
                Sleep(1);
            }
        }

        EnterCriticalSection(&workersCs);
        reapFinishedWorkers(workers);
        LeaveCriticalSection(&workersCs);

        Sleep(5);
    }

    EnterCriticalSection(&workersCs);
    reapFinishedWorkers(workers);
    const int activeBeforeShutdown = countActiveWorkers(workers);
    LeaveCriticalSection(&workersCs);

    WaitForSingleObject(hMutex, INFINITE);
    data->done = true;
    ReleaseMutex(hMutex);

    // Wake all possible workers so they can observe done/stop flags and exit.
    ReleaseSemaphore(hSem, MAX_WORKERS, NULL);

    for (;;) {
        EnterCriticalSection(&workersCs);
        reapFinishedWorkers(workers);
        const int active = countActiveWorkers(workers);
        LeaveCriticalSection(&workersCs);

        if (active == 0)
            break;
        Sleep(20);
    }

    stopController = true;
    if (hControlThread) {
        WaitForSingleObject(hControlThread, INFINITE);
        CloseHandle(hControlThread);
    }

    cout << "\nPer-file results (prime numbers):\n";
    for (int i = 0; i < data->totalFiles; ++i) {
        cout << "[" << (i + 1) << "/" << data->totalFiles << "] " << fileNameOnly(data->filePaths[i]);
        if (data->fileMinPrime[i] == LLONG_MAX) {
            cout << " -> no primes\n";
        } else {
            cout << " -> min=" << data->fileMinPrime[i] << ", max=" << data->fileMaxPrime[i] << "\n";
        }
    }

    cout << "\nGlobal results:\n";
    if (data->globalMin == LLONG_MAX) {
        cout << "No primes found in any file\n";
    } else {
        cout << "Global smallest prime: " << data->globalMin << "\n";
        cout << "Global largest prime: " << data->globalMax << "\n";
    }

    EnterCriticalSection(&workersCs);
    const int finalActiveCount = countActiveWorkers(workers);
    const int totalSpawned = totalSpawnedWorkers;
    LeaveCriticalSection(&workersCs);
    cout << "Active workers before shutdown: " << activeBeforeShutdown << "\n";
    cout << "Total worker processes spawned: " << totalSpawned << "\n";
    cout << "Final active worker count (after shutdown): " << finalActiveCount << "\n";

    DeleteCriticalSection(&workersCs);
    UnmapViewOfFile(data);
    CloseHandle(hMap);
    CloseHandle(hMutex);
    CloseHandle(hSem);
    return 0;
}