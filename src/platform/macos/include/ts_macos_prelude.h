#pragma once

// Force-included into every C++ translation unit of the macOS build.
//
// Apple's libc++ lacks a few C++20 library features the shared sources use
// (MSVC's standard library has them): std::atomic<std::shared_ptr<T>>,
// std::atomic of non-trivially-copyable std::pair, and std::jthread /
// std::stop_token / std::stop_callback. Minimal implementations follow; each is
// only provided when the standard library does not have it.

#ifdef __cplusplus

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <version>

namespace ts_macos_detail {

struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    }
    void unlock() noexcept { flag.clear(std::memory_order_release); }
};

} // namespace ts_macos_detail

namespace std {

#if !defined(__cpp_lib_atomic_shared_ptr)
template <class T> struct atomic<shared_ptr<T>> {
    using value_type = shared_ptr<T>;
    static constexpr bool is_always_lock_free = false;

    atomic() noexcept = default;
    constexpr atomic(nullptr_t) noexcept {}
    atomic(shared_ptr<T> desired) noexcept : value_(std::move(desired)) {}
    atomic(const atomic&) = delete;
    void operator=(const atomic&) = delete;

    bool is_lock_free() const noexcept { return false; }

    void store(shared_ptr<T> desired, memory_order = memory_order_seq_cst) noexcept {
        {
            lock_guard<ts_macos_detail::SpinLock> guard(lock_);
            value_.swap(desired);
        }
        // The previous value is released here, outside the lock.
    }

    shared_ptr<T> load(memory_order = memory_order_seq_cst) const noexcept {
        lock_guard<ts_macos_detail::SpinLock> guard(lock_);
        return value_;
    }

    operator shared_ptr<T>() const noexcept { return load(); }
    void operator=(shared_ptr<T> desired) noexcept { store(std::move(desired)); }

    shared_ptr<T> exchange(shared_ptr<T> desired, memory_order = memory_order_seq_cst) noexcept {
        lock_guard<ts_macos_detail::SpinLock> guard(lock_);
        value_.swap(desired);
        return desired;
    }

    bool compare_exchange_strong(shared_ptr<T>& expected, shared_ptr<T> desired, memory_order = memory_order_seq_cst) noexcept {
        shared_ptr<T> previous;
        {
            lock_guard<ts_macos_detail::SpinLock> guard(lock_);
            const bool same = value_ == expected && !value_.owner_before(expected) && !expected.owner_before(value_);
            if (same) {
                previous = std::move(value_);
                value_ = std::move(desired);
                return true;
            }
            expected = value_;
        }
        return false;
    }
    bool compare_exchange_strong(shared_ptr<T>& expected, shared_ptr<T> desired, memory_order, memory_order) noexcept {
        return compare_exchange_strong(expected, std::move(desired));
    }
    bool compare_exchange_weak(shared_ptr<T>& expected, shared_ptr<T> desired, memory_order = memory_order_seq_cst) noexcept {
        return compare_exchange_strong(expected, std::move(desired));
    }
    bool compare_exchange_weak(shared_ptr<T>& expected, shared_ptr<T> desired, memory_order, memory_order) noexcept {
        return compare_exchange_strong(expected, std::move(desired));
    }

private:
    mutable ts_macos_detail::SpinLock lock_;
    shared_ptr<T> value_;
};
#endif

// libc++'s std::pair is not trivially copyable, so std::atomic<std::pair> is
// rejected; MSVC accepts it.
template <class A, class B> struct atomic<pair<A, B>> {
    using value_type = pair<A, B>;
    static constexpr bool is_always_lock_free = false;

    atomic() noexcept = default;
    constexpr atomic(pair<A, B> desired) noexcept : value_(desired) {}
    atomic(const atomic&) = delete;
    void operator=(const atomic&) = delete;

    bool is_lock_free() const noexcept { return false; }

    void store(pair<A, B> desired, memory_order = memory_order_seq_cst) noexcept {
        lock_guard<ts_macos_detail::SpinLock> guard(lock_);
        value_ = desired;
    }
    pair<A, B> load(memory_order = memory_order_seq_cst) const noexcept {
        lock_guard<ts_macos_detail::SpinLock> guard(lock_);
        return value_;
    }
    operator pair<A, B>() const noexcept { return load(); }
    pair<A, B> operator=(pair<A, B> desired) noexcept {
        store(desired);
        return desired;
    }
    pair<A, B> exchange(pair<A, B> desired, memory_order = memory_order_seq_cst) noexcept {
        lock_guard<ts_macos_detail::SpinLock> guard(lock_);
        pair<A, B> previous = value_;
        value_ = desired;
        return previous;
    }
    bool compare_exchange_strong(pair<A, B>& expected, pair<A, B> desired, memory_order = memory_order_seq_cst) noexcept {
        lock_guard<ts_macos_detail::SpinLock> guard(lock_);
        if (value_ == expected) {
            value_ = desired;
            return true;
        }
        expected = value_;
        return false;
    }
    bool compare_exchange_strong(pair<A, B>& expected, pair<A, B> desired, memory_order, memory_order) noexcept {
        return compare_exchange_strong(expected, desired);
    }
    bool compare_exchange_weak(pair<A, B>& expected, pair<A, B> desired, memory_order = memory_order_seq_cst) noexcept {
        return compare_exchange_strong(expected, desired);
    }
    bool compare_exchange_weak(pair<A, B>& expected, pair<A, B> desired, memory_order, memory_order) noexcept {
        return compare_exchange_strong(expected, desired);
    }

private:
    mutable ts_macos_detail::SpinLock lock_;
    pair<A, B> value_{};
};

#if !defined(__cpp_lib_jthread)

struct nostopstate_t {
    explicit nostopstate_t() = default;
};
inline constexpr nostopstate_t nostopstate{};

namespace ts_stop_detail {

struct StopState {
    // Callbacks run under this lock so a stop_callback being destroyed on
    // another thread waits for its callback to finish.
    std::recursive_mutex mutex;
    std::atomic<bool> requested{ false };
    std::list<std::function<void()>*> callbacks;
};

} // namespace ts_stop_detail

class stop_token {
public:
    stop_token() noexcept = default;
    bool stop_requested() const noexcept { return state_ && state_->requested.load(memory_order_acquire); }
    bool stop_possible() const noexcept { return static_cast<bool>(state_); }
    friend bool operator==(const stop_token& a, const stop_token& b) noexcept { return a.state_ == b.state_; }
    void swap(stop_token& other) noexcept { state_.swap(other.state_); }

private:
    friend class stop_source;
    template <class> friend class stop_callback;
    explicit stop_token(shared_ptr<ts_stop_detail::StopState> state) noexcept : state_(std::move(state)) {}
    shared_ptr<ts_stop_detail::StopState> state_;
};

class stop_source {
public:
    stop_source() : state_(make_shared<ts_stop_detail::StopState>()) {}
    explicit stop_source(nostopstate_t) noexcept {}

    bool request_stop() noexcept {
        if (!state_) return false;
        lock_guard<std::recursive_mutex> lock(state_->mutex);
        if (state_->requested.exchange(true, memory_order_acq_rel)) return false;
        const list<function<void()>*> callbacks = state_->callbacks;
        for (function<void()>* cb : callbacks) (*cb)();
        return true;
    }

    stop_token get_token() const noexcept { return stop_token(state_); }
    bool stop_requested() const noexcept { return state_ && state_->requested.load(memory_order_acquire); }
    bool stop_possible() const noexcept { return static_cast<bool>(state_); }
    void swap(stop_source& other) noexcept { state_.swap(other.state_); }

private:
    shared_ptr<ts_stop_detail::StopState> state_;
};

template <class Callback> class stop_callback {
public:
    using callback_type = Callback;

    template <class C>
    explicit stop_callback(const stop_token& token, C&& cb) : state_(token.state_), callback_(std::forward<C>(cb)) {
        if (!state_) return;
        bool runNow = false;
        {
            lock_guard<std::recursive_mutex> lock(state_->mutex);
            if (state_->requested.load(memory_order_acquire)) {
                runNow = true;
            } else {
                invoker_ = [this] { callback_(); };
                state_->callbacks.push_back(&invoker_);
                registered_ = true;
            }
        }
        if (runNow) callback_();
    }

    ~stop_callback() {
        if (!state_ || !registered_) return;
        lock_guard<std::recursive_mutex> lock(state_->mutex);
        state_->callbacks.remove(&invoker_);
    }

    stop_callback(const stop_callback&) = delete;
    stop_callback& operator=(const stop_callback&) = delete;

private:
    shared_ptr<ts_stop_detail::StopState> state_;
    Callback callback_;
    function<void()> invoker_;
    bool registered_ = false;
};

template <class Callback> stop_callback(stop_token, Callback) -> stop_callback<Callback>;

class jthread {
public:
    using id = thread::id;
    using native_handle_type = thread::native_handle_type;

    jthread() noexcept : source_(nostopstate) {}

    template <class F, class... Args, class = enable_if_t<!is_same_v<remove_cvref_t<F>, jthread>>>
    explicit jthread(F&& f, Args&&... args) : source_() {
        if constexpr (is_invocable_v<decay_t<F>, stop_token, decay_t<Args>...>) {
            thread_ = thread(std::forward<F>(f), source_.get_token(), std::forward<Args>(args)...);
        } else {
            thread_ = thread(std::forward<F>(f), std::forward<Args>(args)...);
        }
    }

    ~jthread() {
        if (joinable()) {
            request_stop();
            join();
        }
    }

    jthread(const jthread&) = delete;
    jthread& operator=(const jthread&) = delete;
    jthread(jthread&& other) noexcept : source_(std::move(other.source_)), thread_(std::move(other.thread_)) {}
    jthread& operator=(jthread&& other) noexcept {
        if (joinable()) {
            request_stop();
            join();
        }
        source_ = std::move(other.source_);
        thread_ = std::move(other.thread_);
        return *this;
    }

    bool joinable() const noexcept { return thread_.joinable(); }
    void join() { thread_.join(); }
    void detach() { thread_.detach(); }
    id get_id() const noexcept { return thread_.get_id(); }
    native_handle_type native_handle() { return thread_.native_handle(); }
    stop_source get_stop_source() noexcept { return source_; }
    stop_token get_stop_token() const noexcept { return source_.get_token(); }
    bool request_stop() noexcept { return source_.request_stop(); }
    void swap(jthread& other) noexcept {
        source_.swap(other.source_);
        thread_.swap(other.thread_);
    }
    static unsigned hardware_concurrency() noexcept { return thread::hardware_concurrency(); }

private:
    stop_source source_;
    thread thread_;
};

#endif // !__cpp_lib_jthread

} // namespace std

#endif // __cplusplus
