#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>

#include <engine/condition_variable.hpp>
#include <engine/mutex.hpp>
#include <engine/sleep.hpp>
#include <logging/log.hpp>
#include <utest/utest.hpp>
#include <utils/periodic_task.hpp>

namespace {
/* Scheduler is dumb, but the life is short. */
const auto kSlowRatio = 5;
}  // namespace

TEST(PeriodicTask, StartStop) {
  TestInCoro([] {
    utils::PeriodicTask task("task", std::chrono::milliseconds(100), [] {});

    task.Stop();
  });
}

namespace {
struct SimpleTaskData {
  engine::Mutex mutex;
  engine::ConditionVariable cv;
  long c = 0;
  long sleep_ms = 0;
  bool throw_exception = false;

  std::function<void()> GetTaskFunction() {
    return std::bind(&SimpleTaskData::Run, this);
  }

  void Run() {
    engine::SleepFor(std::chrono::milliseconds(sleep_ms));
    std::unique_lock<engine::Mutex> lock(mutex);
    c++;
    LOG_DEBUG() << "SimpleTaskData::Run";
    cv.NotifyOne();

    if (throw_exception) throw std::runtime_error("");
  }

  int GetCount() const { return c; }

  template <typename Duration, typename Pred>
  auto WaitFor(Duration duration, Pred pred) {
    std::unique_lock<engine::Mutex> lock(mutex);
    return cv.WaitFor(lock, duration, pred);
  }
};
}  // namespace

TEST(PeriodicTask, SingleRun) {
  TestInCoro([] {
    SimpleTaskData simple;

    utils::PeriodicTask task("task", std::chrono::milliseconds(10),
                             simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(std::chrono::milliseconds(100),
                               [&simple]() { return simple.GetCount() > 0; }));
    task.Stop();
  });
}

TEST(PeriodicTask, MultipleRun) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period = std::chrono::milliseconds(3);
    auto n = 10;
    utils::PeriodicTask task("task", period, simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(period * n * kSlowRatio, [&simple, n]() {
      return simple.GetCount() > n;
    }));
    task.Stop();
  });
}

TEST(PeriodicTask, Period) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period = std::chrono::milliseconds(3);
    auto n = 10;

    auto start = std::chrono::steady_clock::now();
    utils::PeriodicTask task("task", period, simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(period * n * kSlowRatio, [&simple, n]() {
      return simple.GetCount() > n;
    }));
    auto finish = std::chrono::steady_clock::now();

    EXPECT_GE(finish - start, period * n);

    task.Stop();
  });
}

TEST(PeriodicTask, Slow) {
  TestInCoro([] {
    SimpleTaskData simple;

    simple.sleep_ms = 10;
    auto period = std::chrono::milliseconds(2);
    auto full_period = period + std::chrono::milliseconds(simple.sleep_ms);
    auto n = 5;

    auto start = std::chrono::steady_clock::now();
    utils::PeriodicTask task("task", period, simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(full_period * n * kSlowRatio, [&simple, n]() {
      return simple.GetCount() > n;
    }));
    auto finish = std::chrono::steady_clock::now();

    EXPECT_GE(finish - start, full_period * n);

    task.Stop();
  });
}

TEST(PeriodicTask, Strong) {
  TestInCoro([] {
    SimpleTaskData simple;

    simple.sleep_ms = 30;
    auto period = std::chrono::milliseconds(10);
    auto n = 7;

    auto start = std::chrono::steady_clock::now();
    utils::PeriodicTask task("task",
                             utils::PeriodicTask::Settings(
                                 period, utils::PeriodicTask::Flags::kStrong),
                             simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(
        std::chrono::milliseconds(simple.sleep_ms) * n * kSlowRatio,
        [&simple, n]() { return simple.GetCount() > n; }));
    auto finish = std::chrono::steady_clock::now();

    EXPECT_GE(finish - start, std::chrono::milliseconds(simple.sleep_ms) * n);

    task.Stop();
  });
}

TEST(PeriodicTask, Now) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period = std::chrono::milliseconds(10);

    utils::PeriodicTask task(
        "task",
        utils::PeriodicTask::Settings(period, utils::PeriodicTask::Flags::kNow),
        simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(period / 2,
                               [&simple]() { return simple.GetCount() > 0; }));

    task.Stop();
  });
}

TEST(PeriodicTask, NotNow) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period = std::chrono::milliseconds(50);

    utils::PeriodicTask task("task", period, simple.GetTaskFunction());
    EXPECT_FALSE(simple.WaitFor(period / 2,
                                [&simple]() { return simple.GetCount() > 0; }));

    task.Stop();
  });
}

TEST(PeriodicTask, ExceptionPeriod) {
  TestInCoro([] {
    SimpleTaskData simple;
    simple.throw_exception = true;

    auto period = std::chrono::milliseconds(50);
    utils::PeriodicTask::Settings settings(period,
                                           utils::PeriodicTask::Flags::kNow);
    settings.exception_period = std::chrono::milliseconds(10);

    utils::PeriodicTask task("task", settings, simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(*settings.exception_period * kSlowRatio,
                               [&simple]() { return simple.GetCount() > 0; }));

    task.Stop();
  });
}

TEST(PeriodicTask, SetSettings) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period1 = std::chrono::milliseconds(50);
    utils::PeriodicTask::Settings settings(period1);

    utils::PeriodicTask task("task", settings, simple.GetTaskFunction());

    auto period2 = std::chrono::milliseconds(10);
    settings.period = period2;
    task.SetSettings(settings);

    auto n = 4;
    EXPECT_TRUE(
        simple.WaitFor(period1 + period2 * n * kSlowRatio,
                       [&simple, n]() { return simple.GetCount() > n; }));

    task.Stop();
  });
}

TEST(PeriodicTask, StopStop) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period = std::chrono::milliseconds(5);
    utils::PeriodicTask task("task", period, simple.GetTaskFunction());
    EXPECT_TRUE(simple.WaitFor(period * 5,
                               [&simple]() { return simple.GetCount() > 0; }));
    task.Stop();
    task.Stop();
  });
}

TEST(PeriodicTask, StopDefault) {
  TestInCoro([] {
    SimpleTaskData simple;

    utils::PeriodicTask task;
    task.Stop();
  });
}

TEST(PeriodicTask, Restart) {
  TestInCoro([] {
    SimpleTaskData simple;

    auto period = std::chrono::milliseconds(10);
    auto n = 5;
    utils::PeriodicTask task;
    task.Start("task", period, simple.GetTaskFunction());

    EXPECT_TRUE(simple.WaitFor(period * n * kSlowRatio, [&simple, n]() {
      return simple.GetCount() > n;
    }));
    task.Stop();
  });
}
