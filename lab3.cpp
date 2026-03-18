#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <climits>
#include <cctype>
#include <cstdlib>

using namespace std;

#define MAX_QUEUE 100000

// ================= SHARED DATA =================
struct SharedData {
    long long buffer[MAX_QUEUE];
    int head;
    int tail;
    bool done;

    long long globalMin;
    long long globalMax;

    int filesProcessed;
};

// ================= SAFE QUEUE CLASS =================
class SafeQueue {
private:
    HANDLE mutex;
    HANDLE sem;
    SharedData* data;

public:
    SafeQueue(HANDLE m, HANDLE s, SharedData* d)
        : mutex(m), sem(s), data(d) {}

    void push(long long value)
    {
        WaitForSingleObject(mutex, INFINITE);

        data->buffer[data->tail] = value;
        data->tail = (data->tail + 1) % MAX_QUEUE;

        ReleaseMutex(mutex);
        ReleaseSemaphore(sem, 1, NULL);
    }

    bool pop(long long& value)
    {
        WaitForSingleObject(sem, INFINITE);
        WaitForSingleObject(mutex, INFINITE);

        if (data->head == data->tail && data->done)
        {
            ReleaseMutex(mutex);
            return false;
        }

        value = data->buffer[data->head];
        data->head = (data->head + 1) % MAX_QUEUE;

        ReleaseMutex(mutex);
        return true;
    }
};

// ================= PRIME =================
bool isPrime(long long n)
{
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;

    for (long long i = 3; i * i <= n; i += 2)
        if (n % i == 0)
            return false;

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

    do {
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

// ================= WORKER =================
int runWorker(string mapName, string mutexName, string semName)
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
    HANDLE hMutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
    HANDLE hSem = OpenSemaphoreA(SYNCHRONIZE, FALSE, semName.c_str());

    if (!hMap)
    {
        cout << "Worker map open error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    if (!hMutex)
    {
        cout << "Worker mutex open error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    if (!hSem)
    {
        cout << "Worker semaphore open error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SharedData* data = (SharedData*)MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedData));
    if (!data)
    {
        cout << "Worker map view error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SafeQueue queue(hMutex, hSem, data);

    long long value;

    while (queue.pop(value))
    {
        if (isPrime(value))
        {
            WaitForSingleObject(hMutex, INFINITE);

            if (value < data->globalMin) data->globalMin = value;
            if (value > data->globalMax) data->globalMax = value;

            ReleaseMutex(hMutex);
        }
    }

    return 0;
}

// ================= SPAWN WORKER =================
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
        cout << "Failed to create worker\n";
        return NULL;
    }

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// ================= MAIN =================
int main(int argc, char* argv[])
{
    string mapName = "Local\\MyMap";
    string mutexName = "Local\\MyMutex";
    string semName = "Local\\MySem";
    string inputDir = ".";
    int workerCount = 0;

    // WORKER MODE
    if (argc == 2 && string(argv[1]) == "--worker")
        return runWorker(mapName, mutexName, semName);

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
        cout << "Invalid worker count\n";
        return 1;
    }

    DWORD dirAttr = GetFileAttributesA(inputDir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY))
    {
        cout << "Input directory not found: " << inputDir << endl;
        return 1;
    }

    // ================= INIT SHARED =================
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
        PAGE_READWRITE, 0, sizeof(SharedData), mapName.c_str());

    if (!hMap) {
        cout << "Memory error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SharedData* data = (SharedData*)MapViewOfFile(
        hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedData));

    if (!data)
    {
        cout << "MapViewOfFile error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    data->head = data->tail = 0;
    data->done = false;
    data->globalMin = LLONG_MAX;
    data->globalMax = LLONG_MIN;
    data->filesProcessed = 0;

    HANDLE hMutex = CreateMutexA(NULL, FALSE, mutexName.c_str());
    HANDLE hSem = CreateSemaphoreA(NULL, 0, MAX_QUEUE, semName.c_str());

    if (!hMutex || !hSem)
    {
        cout << "Sync object creation error, GetLastError = " << GetLastError() << endl;
        return 1;
    }

    SafeQueue queue(hMutex, hSem, data);

    // ================= PRODUCER =================
    vector<string> txtFiles;
    collectTxtFilesRecursive(inputDir, txtFiles);

    if (txtFiles.empty())
    {
        cout << "No files found in: " << inputDir << endl;
        return 1;
    }

    cout << "Found " << txtFiles.size() << " txt files" << endl;
    cout << "Using " << workerCount << " workers" << endl;

    // ================= START WORKERS =================
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

    long long overallMinPrime = LLONG_MAX;
    long long overallMaxPrime = LLONG_MIN;

    int fileIndex = 0;
    for (const string& filePath : txtFiles)
    {
        fileIndex++;

        HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE) continue;

        char buffer[1024];
        DWORD bytesRead;
        string content;

        while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
            content.append(buffer, bytesRead);

        CloseHandle(hFile);

        long long num = 0;
        bool reading = false;
        long long fileMinPrime = LLONG_MAX;
        long long fileMaxPrime = LLONG_MIN;

        for (char c : content)
        {
            if (isdigit((unsigned char)c))
            {
                num = num * 10 + (c - '0');
                reading = true;
            }
            else if (reading)
            {
                queue.push(num);

                if (isPrime(num))
                {
                    if (num < fileMinPrime) fileMinPrime = num;
                    if (num > fileMaxPrime) fileMaxPrime = num;
                }

                num = 0;
                reading = false;
            }
        }

        if (reading)
        {
            queue.push(num);

            if (isPrime(num))
            {
                if (num < fileMinPrime) fileMinPrime = num;
                if (num > fileMaxPrime) fileMaxPrime = num;
            }
        }

        if (fileMinPrime == LLONG_MAX)
        {
            cout << "[FILE " << fileIndex << "/" << txtFiles.size() << "] "
                 << fileNameOnly(filePath) << " -> no primes found" << endl;
        }
        else
        {
            cout << "[FILE " << fileIndex << "/" << txtFiles.size() << "] "
                 << fileNameOnly(filePath) << " -> min prime: " << fileMinPrime
                 << ", max prime: " << fileMaxPrime << endl;

            if (fileMinPrime < overallMinPrime) overallMinPrime = fileMinPrime;
            if (fileMaxPrime > overallMaxPrime) overallMaxPrime = fileMaxPrime;
        }

        WaitForSingleObject(hMutex, INFINITE);
        data->filesProcessed++;
        ReleaseMutex(hMutex);
    }

    // ================= FINISH =================
    WaitForSingleObject(hMutex, INFINITE);
    data->done = true;
    ReleaseMutex(hMutex);

    ReleaseSemaphore(hSem, workerCount, NULL);

    // ================= WAIT =================
    for (HANDLE h : workers)
        WaitForSingleObject(h, INFINITE);

    // ================= RESULT =================
    cout << "Summary for all files:" << endl;
    if (overallMinPrime == LLONG_MAX)
        cout << "No primes found" << endl;
    else
    {
        cout << "Overall smallest prime: " << overallMinPrime << endl;
        cout << "Overall biggest prime: " << overallMaxPrime << endl;
    }

    return 0;
}
