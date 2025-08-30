Copyright © 2025 Cadell Richard Anderson

// =================================================================
// JobManager.h
// =================================================================
#pragma once
#include <string>
#include <vector>
#include <future>
#include <mutex>
#include <functional>

struct Job {
    int id;
    std::string command;
    std::future<std::string> future;
    bool isDone = false;
    std::string result;
};

class JobManager {
public:
    static std::vector<Job> jobs;
    static int nextJobId;
    static std::mutex jobMutex;
    static int addJob(const std::string& command, std::function<std::string()> task);
    static std::string listJobs();
    static std::string waitForJob(int jobId);
    static void checkJobs();
    static void SubmitJob(std::function<void()> job);
    static void Initialize();
    static void Shutdown();
};
