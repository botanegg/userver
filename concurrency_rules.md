# Concurrency: std → userver

Summary of replacement rules for standard C++ concurrency constructs with userver
equivalents (coroutine-based framework, `engine::` / `concurrent::`). Details in
`scripts/docs/en/userver/intro.md` and `scripts/docs/en/userver/synchronization.md`.

## Forbidden in coroutine code

- std/libc synchronization primitives and blocking I/O (`std::mutex`,
  `std::condition_variable`, `std::this_thread::sleep_for`, blocking sockets, etc.):
  they block the OS thread and stall other coroutines (intro.md:10-22).
- `utils::SwappingSmart` — UB, use `rcu::Variable` instead.
- `std::thread_local` — use `engine::TaskLocalVariable` /
  `engine::TaskInheritedVariable` / `compiler::ThreadLocal` instead.
- `std::atomic` — allowed only for small trivially-copyable types, `seq_cst` only.

If std primitives are unavoidable (third-party library), run them on a dedicated
TaskProcessor (see `scripts/docs/en/userver/task_processors_guide.md`).

## Replacement table

| std | userver | Header |
|---|---|---|
| `std::thread`, `std::async` | `utils::Async()` → `engine::TaskWithResult<T>`; `utils::CriticalAsync`, `engine::AsyncNoTracing` | `core/include/userver/utils/async.hpp` |
| `std::future`/`std::promise` | `engine::TaskWithResult<T>` (or `engine::Future`/`engine::Promise`) | `core/include/userver/engine/future.hpp` |
| `std::thread::join()` | `task.Get()` / `task.Wait()` / `WaitNothrow()` | `core/include/userver/engine/task/task_with_result.hpp` |
| waiting for one of several | `engine::MakeWaitAny` + `WaitAnyContext::Wait`, `engine::WaitAllChecked`, `engine::GetAll` | `core/include/userver/engine/wait_any.hpp`, `wait_all_checked.hpp`, `get_all.hpp` |
| `std::mutex` | `engine::Mutex` (prefer `concurrent::Variable`) | `core/include/userver/engine/mutex.hpp`, `core/include/userver/concurrent/variable.hpp` |
| `std::shared_mutex` | `engine::SharedMutex`; read-mostly — `rcu::Variable`/`rcu::RcuMap` | `core/include/userver/engine/shared_mutex.hpp`, `core/include/userver/rcu/rcu.hpp` |
| `std::condition_variable` | `engine::ConditionVariable`; simple signal — `engine::SingleConsumerEvent`/`MultiConsumerEvent`/`SingleUseEvent` | `core/include/userver/engine/condition_variable.hpp`, `engine/single_consumer_event.hpp`, `engine/multi_consumer_event.hpp`, `engine/single_use_event.hpp` |
| `std::counting_semaphore` | `engine::Semaphore` (ignores cancellation) / `engine::CancellableSemaphore`; RAII — `engine::SemaphoreLock` | `core/include/userver/engine/semaphore.hpp` |
| `std::this_thread::sleep_for/until` | `engine::SleepFor`/`SleepUntil` (ignore cancellation), `engine::InterruptibleSleepFor` | `core/include/userver/engine/sleep.hpp` |
| `std::this_thread::yield` | `engine::Yield()` | `core/include/userver/engine/sleep.hpp` |
| `thread_local` | `engine::TaskLocalVariable` / `engine::TaskInheritedVariable` / `compiler::ThreadLocal` | `core/include/userver/engine/task/local_variable.hpp`, `engine/task/inherited_variable.hpp` |
| queue + mutex + cv | `concurrent::MpscQueue<T>`; variants: `SpscQueue`, `SpmcQueue`, `NonFifo*`, `Unbounded*` | `core/include/userver/concurrent/mpsc_queue.hpp`, `core/include/userver/concurrent/queue.hpp` |
| shared data under a mutex | `concurrent::Variable<T>` | `core/include/userver/concurrent/variable.hpp` |
| `std::map`/`unordered_map` + mutex | `rcu::RcuMap<K,V>` / `concurrent::Variable<...>` | `core/include/userver/rcu/rcu_map.hpp` |
| detached thread | `concurrent::BackgroundTaskStorage::AsyncDetach`/`CriticalAsyncDetach` + `CancelAndWait`; `utils::AsyncBackground` | `core/include/userver/concurrent/background_task_storage.hpp` |
| periodic thread | `utils::PeriodicTask` + `utils::StartPeriodicTask` | `core/include/userver/utils/periodic_task.hpp` |
| POSIX sockets | `engine::io::Socket` (`ReadSome`/`SendAll` with `engine::Deadline`) | `core/include/userver/engine/io/socket.hpp` |
| `std::filesystem` | `::fs::*` (not `::fs::blocking::*`) | `core/include/userver/fs/` |
| own thread pool | TaskProcessor, `components_manager.task_processors` section in static config | `core/include/userver/engine/task/task_processor_fwd.hpp` |

## Idioms

```cpp
auto task = utils::Async("job", SomeFunc, arg);   // like std::async
auto result = task.Get();                          // rethrows the task's exception

engine::Mutex mutex;
{ std::lock_guard<engine::Mutex> lock(mutex); /* ... */ }

concurrent::Variable<std::vector<int>> data;
{ auto view = data.UniqueLock(); view->push_back(1); }

engine::Mutex m; engine::ConditionVariable cv; bool ok = false;
// waiter: cv.Wait(lock, [&ok] { return ok; });  notifier: ok = true; cv.NotifyAll();

engine::Semaphore sema(3);
{ std::shared_lock lock(sema); /* no more than 3 at a time */ }

auto queue = concurrent::MpscQueue<int>::Create();
auto producer = queue->GetProducer(); auto consumer = queue->GetConsumer();
// producer.Push(v, deadline); consumer.Pop(v, deadline) == false — no producers left

concurrent::BackgroundTaskStorage bts;   // class member, declared after used fields
bts.AsyncDetach("job", [&] { /* ... */ });
bts.CancelAndWait();

utils::StartPeriodicTask(periodic_task_, "tick",
    utils::PeriodicTask::Settings(std::chrono::seconds(5)), [this] { /* ... */ });
```

Reference snippets: `core/src/engine/*_test.cpp`, `core/src/concurrent/*_test.cpp`,
`core/src/rcu/*_test.cpp` (sections "Sample ... usage").

## Pitfalls

- `engine::Mutex`/`SharedMutex`/`Semaphore` ignore task cancellation; interruptibility —
  `engine::CancellableSemaphore`, `engine::InterruptibleSleepFor` + check
  `engine::current_task::ShouldCancel()`, `engine::SingleConsumerEvent::WaitForEvent` → `false`.
- Wake-up order of waiters is unspecified — do not rely on FIFO.
- `engine::Mutex` must not be re-locked by the same task.
- A task lives no longer than its `Task` object: the destructor cancels and waits for it.
  Fields used by the task must be declared before the task field.
- Background tasks from a handler must use `utils::AsyncBackground`/`AsyncDetach` only:
  an inherited deadline will cancel them (`scripts/docs/en/userver/deadline_propagation.md`).
- `BackgroundTaskStorage::CancelAndWait` — no more than once.
- `concurrent::Producer`/`Consumer` — single thread each; for many — `GetMultiProducer`/`GetMultiConsumer`.
- `engine::SharedMutex` is not always faster than `engine::Mutex`; prefer `rcu` for read-mostly.
- Deprecated: `engine::AsyncNoSpan` → `AsyncNoTracing`; `engine::RunInCoro` →
  `RunStandalone`; `engine::WaitAny*` → `MakeWaitAny` + `WaitAnyContext::Wait`.
- `engine::Future` — coroutine threads only; `engine::Promise` — from anywhere.
- `BackgroundTaskStorage` is not a replacement for `std::vector<Task>` in handlers.

## Sources

- `scripts/docs/en/userver/intro.md` — basics, replacement table, flavors of async
- `scripts/docs/en/userver/synchronization.md` — synchronization, atomic, thread_local
- `scripts/docs/en/userver/task_processors_guide.md` — TaskProcessors
- `scripts/docs/en/userver/periodics.md` — `utils::PeriodicTask`, DistLock
- `scripts/docs/en/userver/faq.md` — capture lifetimes, CancelAndWait
- `scripts/docs/en/userver/deadline_propagation.md` — deadlines and background tasks
