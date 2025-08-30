Copyright © 2025 Cadell Richard Anderson

// =================================================================
// JobManager.cpp
// =================================================================
#include "JobManager.h"
#include <iostream>
#include <sstream>
#include <queue>
#include <condition_variable>

std::vector<Job> JobManager::jobs;
int JobManager::nextJobId = 1;
std::mutex JobManager::jobMutex;

namespace {
    std::queue<std::function<void()>> void_jobs;
    std::mutex void_jobMutex;
    std::condition_variable void_jobCV;
    bool void_running = true;
    std::thread void_worker;

    void ProcessVoidJobs() {
        while (void_running) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(void_jobMutex);
                void_jobCV.wait(lock, [] { return !void_jobs.empty() || !void_running; });
                if (!void_running && void_jobs.empty()) break;
                job = std::move(void_jobs.front());
                void_jobs.pop();
            }
            job();
        }
    }
}

int JobManager::addJob(const std::string& command, std::function<std::string()> task) {
    std::lock_guard<std::mutex> lock(jobMutex);
    int id = nextJobId++;
    jobs.emplace_back(Job{ id, command, std::async(std::launch::async, task), false, "" });
    return id;
}

std::string JobManager::listJobs() {
    std::lock_guard<std::mutex> lock(jobMutex);
    std::stringstream ss;
    for (const auto& job : jobs) {
        ss << "[" << job.id << "]" << (job.isDone ? "  Done" : "+ Running") << "    " << job.command << "\n";
    }
    return ss.str();
}

std::string JobManager::waitForJob(int jobId) {
    std::unique_lock<std::mutex> lock(jobMutex);
    auto it = std::find_if(jobs.begin(), jobs.end(), [jobId](const Job& j) { return j.id == jobId; });
    if (it != jobs.end()) {
        lock.unlock(); // Unlock before waiting
        std::string result = it->future.get();
        lock.lock(); // Re-lock to modify the vector
        jobs.erase(it);
        return result;
    }
    return "Error: Job not found.";
}

void JobManager::checkJobs() {
    std::lock_guard<std::mutex> lock(jobMutex);
    for (auto& job : jobs) {
        if (!job.isDone && job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            job.isDone = true;
            job.result = job.future.get();
            std::cout << "\n[Job " << job.id << "+ Done] " << job.command << "\n" << job.result << std::endl;
        }
    }
    // Clean up completed jobs
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const Job& j) { return j.isDone; }), jobs.end());
}

void JobManager::SubmitJob(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(void_jobMutex);
        void_jobs.push(std::move(job));
    }
    void_jobCV.notify_one();
}

void JobManager::Initialize() {
    void_running = true;
    void_worker = std::thread(ProcessVoidJobs);
}

void JobManager::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(void_jobMutex);
        void_running = false;
    }
    void_jobCV.notify_all();
    if (void_worker.joinable()) void_worker.join();
}
