#include "ray/raylet/worker_pool.h"

#include <sys/wait.h>

#include <algorithm>
#include <thread>

#include "ray/common/constants.h"
#include "ray/common/ray_config.h"
#include "ray/common/status.h"
#include "ray/stats/stats.h"
#include "ray/util/logging.h"
#include "ray/util/util.h"

namespace {

// A helper function to get a worker from a list.
std::shared_ptr<ray::raylet::Worker> GetWorker(
    const std::unordered_set<std::shared_ptr<ray::raylet::Worker>> &worker_pool,
    const std::shared_ptr<ray::LocalClientConnection> &connection) {
  for (auto it = worker_pool.begin(); it != worker_pool.end(); it++) {
    if ((*it)->Connection() == connection) {
      return (*it);
    }
  }
  return nullptr;
}

// A helper function to remove a worker from a list. Returns true if the worker
// was found and removed.
bool RemoveWorker(std::unordered_set<std::shared_ptr<ray::raylet::Worker>> &worker_pool,
                  const std::shared_ptr<ray::raylet::Worker> &worker) {
  return worker_pool.erase(worker) > 0;
}

}  // namespace

namespace ray {

namespace raylet {

/// A constructor that initializes a worker pool with
/// (num_worker_processes * states_by_lang_[language].num_workers_per_process) workers for
/// each language.
WorkerPool::WorkerPool(int num_worker_processes, int maximum_startup_concurrency,
                       std::shared_ptr<gcs::RedisGcsClient> gcs_client,
                       const WorkerCommandMap &worker_commands)
    : maximum_startup_concurrency_(maximum_startup_concurrency),
      gcs_client_(std::move(gcs_client)) {
  RAY_CHECK(maximum_startup_concurrency > 0);
  // Ignore SIGCHLD signals. If we don't do this, then worker processes will
  // become zombies instead of dying gracefully.
  signal(SIGCHLD, SIG_IGN);
  for (const auto &entry : worker_commands) {
    // Initialize the pool state for this language.
    auto &state = states_by_lang_[entry.first];
    switch (entry.first) {
    case Language::PYTHON:
      state.num_workers_per_process =
          RayConfig::instance().num_workers_per_process_python();
      break;
    case Language::JAVA:
      state.num_workers_per_process =
          RayConfig::instance().num_workers_per_process_java();
      break;
    default:
      RAY_LOG(FATAL) << "The number of workers per process for "
                     << Language_Name(entry.first) << " worker is not set.";
    }
    RAY_CHECK(state.num_workers_per_process > 0)
        << "Number of workers per process of language " << Language_Name(entry.first)
        << " must be positive.";
    state.multiple_for_warning =
        std::max(num_worker_processes, maximum_startup_concurrency) *
        state.num_workers_per_process;
    // Set worker command for this language.
    state.worker_command = entry.second;
    RAY_CHECK(!state.worker_command.empty()) << "Worker command must not be empty.";
    // Force-start num_workers worker processes for this language.
    for (int i = 0; i < num_worker_processes; i++) {
      StartWorkerProcess(entry.first);
    }
  }
}

WorkerPool::~WorkerPool() {
  std::unordered_set<pid_t> pids_to_kill;
  for (const auto &entry : states_by_lang_) {
    // Kill all registered workers. NOTE(swang): This assumes that the registered
    // workers were started by the pool.
    for (const auto &worker : entry.second.registered_workers) {
      pids_to_kill.insert(worker->Pid());
    }
    // Kill all the workers that have been started but not registered.
    for (const auto &starting_worker : entry.second.starting_worker_processes) {
      pids_to_kill.insert(starting_worker.first);
    }
  }
  for (const auto &pid : pids_to_kill) {
    RAY_CHECK(pid > 0);
    kill(pid, SIGKILL);
  }
  // Waiting for the workers to be killed
  for (const auto &pid : pids_to_kill) {
    waitpid(pid, NULL, 0);
  }
}

uint32_t WorkerPool::Size(const Language &language) const {
  const auto state = states_by_lang_.find(language);
  if (state == states_by_lang_.end()) {
    return 0;
  } else {
    return static_cast<uint32_t>(state->second.idle.size() +
                                 state->second.idle_actor.size());
  }
}

int WorkerPool::StartWorkerProcess(const Language &language,
                                   const std::vector<std::string> &dynamic_options) {
  auto &state = GetStateForLanguage(language);
  // If we are already starting up too many workers, then return without starting
  // more.
  if (static_cast<int>(state.starting_worker_processes.size()) >=
      maximum_startup_concurrency_) {
    // Workers have been started, but not registered. Force start disabled -- returning.
    RAY_LOG(DEBUG) << "Worker not started, " << state.starting_worker_processes.size()
                   << " worker processes of language type " << static_cast<int>(language)
                   << " pending registration";
    return -1;
  }
  // Either there are no workers pending registration or the worker start is being forced.
  RAY_LOG(DEBUG) << "Starting new worker process, current pool has "
                 << state.idle_actor.size() << " actor workers, and " << state.idle.size()
                 << " non-actor workers";

  int workers_to_start;
  if (dynamic_options.empty()) {
    workers_to_start = state.num_workers_per_process;
  } else {
    workers_to_start = 1;
  }

  // Extract pointers from the worker command to pass into execvp.
  std::vector<std::string> worker_command_args;
  size_t dynamic_option_index = 0;
  bool num_workers_arg_replaced = false;
  for (auto const &token : state.worker_command) {
    const auto option_placeholder =
        kWorkerDynamicOptionPlaceholderPrefix + std::to_string(dynamic_option_index);

    if (token == option_placeholder) {
      if (!dynamic_options.empty()) {
        RAY_CHECK(dynamic_option_index < dynamic_options.size());
        auto options = SplitStrByWhitespaces(dynamic_options[dynamic_option_index]);
        worker_command_args.insert(worker_command_args.end(), options.begin(),
                                   options.end());
        ++dynamic_option_index;
      }
    } else {
      size_t num_workers_index = token.find(kWorkerNumWorkersPlaceholder);
      if (num_workers_index != std::string::npos) {
        std::string arg = token;
        worker_command_args.push_back(arg.replace(num_workers_index,
                                                  strlen(kWorkerNumWorkersPlaceholder),
                                                  std::to_string(workers_to_start)));
        num_workers_arg_replaced = true;
      } else {
        worker_command_args.push_back(token);
      }
    }
  }
  RAY_CHECK(num_workers_arg_replaced || state.num_workers_per_process == 1)
      << "Expect to start " << state.num_workers_per_process << " workers per "
      << Language_Name(language) << " worker process. But the "
      << kWorkerNumWorkersPlaceholder << "placeholder is not found in worker command.";

  pid_t pid = StartProcess(worker_command_args);
  if (pid < 0) {
    // Failure case.
    RAY_LOG(FATAL) << "Failed to fork worker process: " << strerror(errno);
  } else if (pid > 0) {
    // Parent process case.
    RAY_LOG(DEBUG) << "Started worker process of " << workers_to_start
                   << " worker(s) with pid " << pid;
    state.starting_worker_processes.emplace(pid, workers_to_start);
    return pid;
  }
  return -1;
}

pid_t WorkerPool::StartProcess(const std::vector<std::string> &worker_command_args) {
  if (RAY_LOG_ENABLED(DEBUG)) {
    std::stringstream stream;
    stream << "Starting worker process with command:";
    for (const auto &arg : worker_command_args) {
      stream << " " << arg;
    }
    RAY_LOG(DEBUG) << stream.str();
  }

  // Launch the process to create the worker.
  pid_t pid = fork();

  if (pid != 0) {
    return pid;
  }

  // Child process case.
  // Reset the SIGCHLD handler for the worker.
  signal(SIGCHLD, SIG_DFL);

  // Try to execute the worker command.
  std::vector<const char *> worker_command_args_str;
  for (const auto &arg : worker_command_args) {
    worker_command_args_str.push_back(arg.c_str());
  }
  worker_command_args_str.push_back(nullptr);
  int rv = execvp(worker_command_args_str[0],
                  const_cast<char *const *>(worker_command_args_str.data()));

  // The worker failed to start. This is a fatal error.
  RAY_LOG(FATAL) << "Failed to start worker with return value " << rv << ": "
                 << strerror(errno);
  return 0;
}

Status WorkerPool::RegisterWorker(const std::shared_ptr<Worker> &worker) {
  const auto pid = worker->Pid();
  const auto port = worker->Port();
  RAY_LOG(DEBUG) << "Registering worker with pid " << pid << ", port: " << port;
  auto &state = GetStateForLanguage(worker->GetLanguage());

  auto it = state.starting_worker_processes.find(pid);
  if (it == state.starting_worker_processes.end()) {
    RAY_LOG(WARNING) << "Received a register request from an unknown worker " << pid;
    return Status::Invalid("Unknown worker");
  }
  it->second--;
  if (it->second == 0) {
    state.starting_worker_processes.erase(it);
  }

  state.registered_workers.emplace(std::move(worker));
  return Status::OK();
}

Status WorkerPool::RegisterDriver(const std::shared_ptr<Worker> &driver) {
  RAY_CHECK(!driver->GetAssignedTaskId().IsNil());
  auto &state = GetStateForLanguage(driver->GetLanguage());
  state.registered_drivers.insert(std::move(driver));
  return Status::OK();
}

std::shared_ptr<Worker> WorkerPool::GetRegisteredWorker(
    const std::shared_ptr<LocalClientConnection> &connection) const {
  for (const auto &entry : states_by_lang_) {
    auto worker = GetWorker(entry.second.registered_workers, connection);
    if (worker != nullptr) {
      return worker;
    }
  }
  return nullptr;
}

std::shared_ptr<Worker> WorkerPool::GetRegisteredDriver(
    const std::shared_ptr<LocalClientConnection> &connection) const {
  for (const auto &entry : states_by_lang_) {
    auto driver = GetWorker(entry.second.registered_drivers, connection);
    if (driver != nullptr) {
      return driver;
    }
  }
  return nullptr;
}

void WorkerPool::PushWorker(const std::shared_ptr<Worker> &worker) {
  // Since the worker is now idle, unset its assigned task ID.
  RAY_CHECK(worker->GetAssignedTaskId().IsNil())
      << "Idle workers cannot have an assigned task ID";
  auto &state = GetStateForLanguage(worker->GetLanguage());

  auto it = state.dedicated_workers_to_tasks.find(worker->Pid());
  if (it != state.dedicated_workers_to_tasks.end()) {
    // The worker is used for the actor creation task with dynamic options.
    // Put it into idle dedicated worker pool.
    const auto task_id = it->second;
    state.idle_dedicated_workers[task_id] = std::move(worker);
  } else {
    // The worker is not used for the actor creation task without dynamic options.
    // Put the worker to the corresponding idle pool.
    if (worker->GetActorId().IsNil()) {
      state.idle.insert(std::move(worker));
    } else {
      state.idle_actor[worker->GetActorId()] = std::move(worker);
    }
  }
}

std::shared_ptr<Worker> WorkerPool::PopWorker(const TaskSpecification &task_spec) {
  auto &state = GetStateForLanguage(task_spec.GetLanguage());

  std::shared_ptr<Worker> worker = nullptr;
  int pid = -1;
  if (task_spec.IsActorCreationTask() && !task_spec.DynamicWorkerOptions().empty()) {
    // Code path of actor creation task with dynamic worker options.
    // Try to pop it from idle dedicated pool.
    auto it = state.idle_dedicated_workers.find(task_spec.TaskId());
    if (it != state.idle_dedicated_workers.end()) {
      // There is an idle dedicated worker for this task.
      worker = std::move(it->second);
      state.idle_dedicated_workers.erase(it);
      // Because we found a worker that can perform this task,
      // we can remove it from dedicated_workers_to_tasks.
      state.dedicated_workers_to_tasks.erase(worker->Pid());
      state.tasks_to_dedicated_workers.erase(task_spec.TaskId());
    } else if (!HasPendingWorkerForTask(task_spec.GetLanguage(), task_spec.TaskId())) {
      // We are not pending a registration from a worker for this task,
      // so start a new worker process for this task.
      pid = StartWorkerProcess(task_spec.GetLanguage(), task_spec.DynamicWorkerOptions());
      if (pid > 0) {
        state.dedicated_workers_to_tasks[pid] = task_spec.TaskId();
        state.tasks_to_dedicated_workers[task_spec.TaskId()] = pid;
      }
    }
  } else if (!task_spec.IsActorTask()) {
    // Code path of normal task or actor creation task without dynamic worker options.
    if (!state.idle.empty()) {
      worker = std::move(*state.idle.begin());
      state.idle.erase(state.idle.begin());
    } else {
      // There are no more non-actor workers available to execute this task.
      // Start a new worker process.
      pid = StartWorkerProcess(task_spec.GetLanguage());
    }
  } else {
    // Code path of actor task.
    const auto &actor_id = task_spec.ActorId();
    auto actor_entry = state.idle_actor.find(actor_id);
    if (actor_entry != state.idle_actor.end()) {
      worker = std::move(actor_entry->second);
      state.idle_actor.erase(actor_entry);
    }
  }

  if (worker == nullptr && pid > 0) {
    WarnAboutSize();
  }

  return worker;
}

bool WorkerPool::DisconnectWorker(const std::shared_ptr<Worker> &worker) {
  auto &state = GetStateForLanguage(worker->GetLanguage());
  RAY_CHECK(RemoveWorker(state.registered_workers, worker));

  stats::CurrentWorker().Record(
      0, {{stats::LanguageKey, Language_Name(worker->GetLanguage())},
          {stats::WorkerPidKey, std::to_string(worker->Pid())}});

  return RemoveWorker(state.idle, worker);
}

void WorkerPool::DisconnectDriver(const std::shared_ptr<Worker> &driver) {
  auto &state = GetStateForLanguage(driver->GetLanguage());
  RAY_CHECK(RemoveWorker(state.registered_drivers, driver));
  stats::CurrentDriver().Record(
      0, {{stats::LanguageKey, Language_Name(driver->GetLanguage())},
          {stats::WorkerPidKey, std::to_string(driver->Pid())}});
}

inline WorkerPool::State &WorkerPool::GetStateForLanguage(const Language &language) {
  auto state = states_by_lang_.find(language);
  RAY_CHECK(state != states_by_lang_.end()) << "Required Language isn't supported.";
  return state->second;
}

std::vector<std::shared_ptr<Worker>> WorkerPool::GetWorkersRunningTasksForJob(
    const JobID &job_id) const {
  std::vector<std::shared_ptr<Worker>> workers;

  for (const auto &entry : states_by_lang_) {
    for (const auto &worker : entry.second.registered_workers) {
      if (worker->GetAssignedJobId() == job_id) {
        workers.push_back(worker);
      }
    }
  }

  return workers;
}

void WorkerPool::WarnAboutSize() {
  for (const auto &entry : states_by_lang_) {
    auto state = entry.second;
    int64_t num_workers_started_or_registered = 0;
    num_workers_started_or_registered +=
        static_cast<int64_t>(state.registered_workers.size());
    for (const auto &starting_process : state.starting_worker_processes) {
      num_workers_started_or_registered += starting_process.second;
    }
    int64_t multiple = num_workers_started_or_registered / state.multiple_for_warning;
    std::stringstream warning_message;
    if (multiple >= 3 && multiple > state.last_warning_multiple) {
      // Push an error message to the user if the worker pool tells us that it is
      // getting too big.
      state.last_warning_multiple = multiple;
      warning_message << "WARNING: " << num_workers_started_or_registered << " "
                      << Language_Name(entry.first)
                      << " workers have been started. This could be a result of using "
                      << "a large number of actors, or it could be a consequence of "
                      << "using nested tasks "
                      << "(see https://github.com/ray-project/ray/issues/3644) for "
                      << "some a discussion of workarounds.";
      RAY_CHECK_OK(gcs_client_->error_table().PushErrorToDriver(
          JobID::Nil(), "worker_pool_large", warning_message.str(), current_time_ms()));
    }
  }
}

bool WorkerPool::HasPendingWorkerForTask(const Language &language,
                                         const TaskID &task_id) {
  auto &state = GetStateForLanguage(language);
  auto it = state.tasks_to_dedicated_workers.find(task_id);
  return it != state.tasks_to_dedicated_workers.end();
}

std::unordered_set<ObjectID> WorkerPool::GetActiveObjectIDs() const {
  std::unordered_set<ObjectID> active_object_ids;
  for (const auto &entry : states_by_lang_) {
    for (const auto &worker : entry.second.registered_workers) {
      active_object_ids.insert(worker->GetActiveObjectIds().begin(),
                               worker->GetActiveObjectIds().end());
    }
    for (const auto &driver : entry.second.registered_drivers) {
      active_object_ids.insert(driver->GetActiveObjectIds().begin(),
                               driver->GetActiveObjectIds().end());
    }
  }
  return active_object_ids;
}

std::string WorkerPool::DebugString() const {
  std::stringstream result;
  result << "WorkerPool:";
  for (const auto &entry : states_by_lang_) {
    result << "\n- num " << Language_Name(entry.first)
           << " workers: " << entry.second.registered_workers.size();
    result << "\n- num " << Language_Name(entry.first)
           << " drivers: " << entry.second.registered_drivers.size();
  }
  return result.str();
}

void WorkerPool::RecordMetrics() const {
  for (const auto &entry : states_by_lang_) {
    // Record worker.
    for (auto worker : entry.second.registered_workers) {
      stats::CurrentWorker().Record(
          worker->Pid(), {{stats::LanguageKey, Language_Name(worker->GetLanguage())},
                          {stats::WorkerPidKey, std::to_string(worker->Pid())}});
    }

    // Record driver.
    for (auto driver : entry.second.registered_drivers) {
      stats::CurrentDriver().Record(
          driver->Pid(), {{stats::LanguageKey, Language_Name(driver->GetLanguage())},
                          {stats::WorkerPidKey, std::to_string(driver->Pid())}});
    }
  }
}

}  // namespace raylet

}  // namespace ray
