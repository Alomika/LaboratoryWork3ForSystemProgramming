#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <climits>
#include <cctype>
#include <cstdlib>
#include <cstring>

using namespace std;

#define MAX_QUEUE 100000
#define MAX_FILES 4096
#define MAX_PATH_LEN 260

struct QueueItem {
    int fileIndex;
};

struct SharedData {
    QueueItem buffer[MAX_QUEUE];
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
};

class SafeQueue {
private:
    HANDLE mutex;
    HANDLE sem;
    SharedData* data;

public:
    SafeQueue(HANDLE m, HANDLE s, SharedData* d)
        : mutex(m), sem(s), data(d) {}

    bool push(const QueueItem& item)
    {
        WaitForSingleObject(mutex, INFINITE);

        if (data->count >= MAX_QUEUE)
        {
            ReleaseMutex(mutex);
            return false;
        }

        data->buffer[data->tail] = item;
        data->tail = (data->tail + 1) % MAX_QUEUE;
        data->count++;

        ReleaseMutex(mutex);
        ReleaseSemaphore(sem, 1, NULL);
        return true;
    }

    bool pop(QueueItem& item)
    {
        WaitForSingleObject(sem, INFINITE);
        WaitForSingleObject(mutex, INFINITE);

        if (data->count == 0 && data->done)
        {
            ReleaseMutex(mutex);
            return false;
        }

        if (data->count == 0)
        {
            ReleaseMutex(mutex);
            return false;
        }

        item = data->buffer[data->head];
        data->head = (data->head + 1) % MAX_QUEUE;
        data->count--;

        ReleaseMutex(mutex);
        return true;
    }
};

bool isPrime(long long n)
{
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;

    for (long long i = 3; i * i <= n; i += 2)
    {
        if (n % i == 0)
            return false;
    }

    return true;
}

bool endsWithTxt(const string& name)
{
    if (name.size() < 4) return false;

    char c1 = (char)tolower((unsigned char)name[name.size() - 4]);
    char c2 = (char)tolower((unsigned char)name[name.size() - 3]);
    char c3 = (char)tolower((unsigned char)name[name.size() - 2]);
    char c4 = (char)tolower((unsigned char)name[name.size() - 1]);

    return c1 == '.' && c2 == 't' && c3 == 'x' && c4 == 't';
}

string fileNameOnly(const string& path)
{
    size_t pos = path.find_last_of("\\/");
    if (pos == string::npos) return path;
    return path.substr(pos + 1);
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

    do
    {
        string name = fd.cFileName;
        if (name == "." || name == "..")
            continue;

        string fullPath = dir;
        if (!fullPath.empty() && fullPath.back() != '\\' && fullPath.back() != '/')
            fullPath += "\\";
        fullPath += name;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            collectTxtFilesRecursive(fullPath, outFiles);
        }
        else if (endsWithTxt(name))
        {
            outFiles.push_back(fullPath);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

bool processOneFile(const char* filePath, long long& minPrime, long long& maxPrime)
{
    HANDLE hFile = CreateFileA(filePath, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    string content;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
    {
        content.append(buffer, bytesRead);
    }
    CloseHandle(hFile);

    minPrime = LLONG_MAX;
    maxPrime = LLONG_MIN;

    long long num = 0;
    bool reading = false;

    for (char c : content)
    {
        if (isdigit((unsigned char)c))
        {
            num = num * 10 + (c - '0');
            reading = true;
        }
        else if (reading)
        {
            if (isPrime(num))
            {
                if (num < minPrime) minPrime = num;
                if (num > maxPrime) maxPrime = num;
            }
            num = 0;
            reading = false;
        }
    }

    if (reading && isPrime(num))
    {
        if (num < minPrime) minPrime = num;
        if (num > maxPrime) maxPrime = num;
    }

    return true;
}

int runWorker(const string& mapName, const string& mutexName, const string& semName)
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
    HANDLE hMutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
    HANDLE hSem = OpenSemaphoreA(SYNCHRONIZE, FALSE, semName.c_str());

    if (!hMap || !hMutex || !hSem)
    {
        cout << "Worker open object error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SharedData* data = (SharedData*)MapViewOfFile(
        hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedData));

    if (!data)
    {
        cout << "Worker map view error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SafeQueue queue(hMutex, hSem, data);

    QueueItem item;
    while (queue.pop(item))
    {
        int idx = item.fileIndex;
        if (idx < 0 || idx >= data->totalFiles)
            continue;

        long long localMin = LLONG_MAX;
        long long localMax = LLONG_MIN;

        bool ok = processOneFile(data->filePaths[idx], localMin, localMax);

        WaitForSingleObject(hMutex, INFINITE);

        if (!ok)
        {
            data->fileMinPrime[idx] = LLONG_MAX;
            data->fileMaxPrime[idx] = LLONG_MIN;
        }
        else
        {
            data->fileMinPrime[idx] = localMin;
            data->fileMaxPrime[idx] = localMax;

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

HANDLE spawnWorker()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    wstring cmd = L"\"" + wstring(exePath) + L"\" --worker";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    if (!CreateProcessW(NULL, (LPWSTR)cmd.c_str(),
        NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        cout << "Failed to create worker" << endl;
        return NULL;
    }

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int main(int argc, char* argv[])
{
    string mapName = "Local\\MyMap";
    string mutexName = "Local\\MyMutex";
    string semName = "Local\\MySem";

    if (argc == 2 && string(argv[1]) == "--worker")
    {
        return runWorker(mapName, mutexName, semName);
    }

    string inputDir = ".";
    int workerCount = 0;

    if (argc >= 2)
        inputDir = argv[1];
    if (argc >= 3)
        workerCount = atoi(argv[2]);

    if (workerCount <= 0)
    {
        cout << "Enter worker count: ";
        cin >> workerCount;
    }

    if (workerCount <= 0)
    {
        cout << "Invalid worker count" << endl;
        return 1;
    }

    DWORD dirAttr = GetFileAttributesA(inputDir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY))
    {
        cout << "Input directory not found: " << inputDir << endl;
        return 1;
    }

    vector<string> txtFiles;
    collectTxtFilesRecursive(inputDir, txtFiles);

    if (txtFiles.empty())
    {
        cout << "No .txt files found in: " << inputDir << endl;
        return 1;
    }

    if (txtFiles.size() > MAX_FILES)
    {
        cout << "Too many files. Max supported: " << MAX_FILES << endl;
        return 1;
    }

    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
        PAGE_READWRITE, 0, sizeof(SharedData), mapName.c_str());

    if (!hMap)
    {
        cout << "Memory mapping error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SharedData* data = (SharedData*)MapViewOfFile(
        hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedData));

    if (!data)
    {
        cout << "MapViewOfFile error, GetLastError = " << GetLastError() << endl;
        CloseHandle(hMap);
        return 1;
    }

    HANDLE hMutex = CreateMutexA(NULL, FALSE, mutexName.c_str());
    HANDLE hSem = CreateSemaphoreA(NULL, 0, MAX_QUEUE, semName.c_str());

    if (!hMutex || !hSem)
    {
        cout << "Sync object creation error, GetLastError = " << GetLastError() << endl;
        UnmapViewOfFile(data);
        CloseHandle(hMap);
        return 1;
    }

    data->head = 0;
    data->tail = 0;
    data->count = 0;
    data->done = false;
    data->totalFiles = (int)txtFiles.size();
    data->globalMin = LLONG_MAX;
    data->globalMax = LLONG_MIN;

    for (int i = 0; i < data->totalFiles; i++)
    {
        ZeroMemory(data->filePaths[i], MAX_PATH_LEN);
        strncpy(data->filePaths[i], txtFiles[i].c_str(), MAX_PATH_LEN - 1);
        data->filePaths[i][MAX_PATH_LEN - 1] = '\0';
        data->fileMinPrime[i] = LLONG_MAX;
        data->fileMaxPrime[i] = LLONG_MIN;
    }

    SafeQueue queue(hMutex, hSem, data);

    cout << "Found " << txtFiles.size() << " txt files" << endl;
    cout << "Using " << workerCount << " workers" << endl;

    vector<HANDLE> workers;
    for (int i = 0; i < workerCount; i++)
    {
        HANDLE hWorker = spawnWorker();
        if (!hWorker)
        {
            cout << "Failed to start worker " << (i + 1) << endl;
            return 1;
        }
        workers.push_back(hWorker);
    }

    for (int i = 0; i < data->totalFiles; i++)
    {
        if (!queue.push({ i }))
        {
            cout << "Queue overflow while scheduling files." << endl;
            break;
        }
    }

 
    WaitForSingleObject(hMutex, INFINITE);
    data->done = true;
    ReleaseMutex(hMutex);

    ReleaseSemaphore(hSem, (LONG)workers.size(), NULL);

    for (HANDLE h : workers)
    {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    cout << "\nPer-file prime numbers:" << endl;
    for (int i = 0; i < data->totalFiles; i++)
    {
        cout << "[FILE " << (i + 1) << "/" << data->totalFiles << "] "
             << fileNameOnly(data->filePaths[i]);

        if (data->fileMinPrime[i] == LLONG_MAX)
        {
            cout << " -> no primes found" << endl;
        }
        else
        {
            cout << " -> min: " << data->fileMinPrime[i]
                 << ", max: " << data->fileMaxPrime[i] << endl;
        }
    }

    cout << "\nSummary:" << endl;
    if (data->globalMin == LLONG_MAX)
    {
        cout << "No primes found" << endl;
    }
    else
    {
        cout << "Overall smallest prime: " << data->globalMin << endl;
        cout << "Overall biggest prime: " << data->globalMax << endl;
    }

    UnmapViewOfFile(data);
    CloseHandle(hMap);
    CloseHandle(hMutex);
    CloseHandle(hSem);

    return 0;
}
