/******************************************************************************
 Copyright (c) 2019, Lukas Bagaric
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 - Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 - Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************

This file defines a single template, spsc_queue, which implements a bounded
queue with at most one producer, and one consumer at the same time.

spsc_queue is intended to be used in environments, where heap-allocation must
never occur. While it is possible to use spsc_queue in real-time environments,
the implementation trades a worse worst-case for a significantly better
average-case.

spsc_queue has highest throughput under contention if:
  * you have small (register sized) elements OR
  * if the total size of the queue (size of element times number of elements)
    will not exceed the size of your processors fastest cache.

spsc_queue takes up to three template parameters:
  * T: The type of a single element
  * queue_size: The number of slots for elements within the queue.
                Note: Due to implementation details, one slot is reserved and
                      cannot be used.
  * align_log2: The number of bytes to align on, expressed as an exponent for
                two, so the actual alignment is (1 << align_log2) bytes. This
                number should be at least log2(alignof(size_t)). Ideal values
                avoid destructive hardware interference (false sharing).
                Default is 7.
                alignof(T) must not be greater than (1 << align_log2).

Interface:
  General:
    bool is_empty() const;
        Returns true if there is currently no object in the queue.
        Returns false otherwise.

    bool is_full() const;
        Returns true if no more objects can be added to the queue.
        Returns false otherwise.

  Enqueue:
    bool push(const T& elem);
    bool push(T&& elem);
        Tries to insert elem into the queue. Returns true if successful, false
        otherwise.

    template<typename InputIterator>
    InputIterator push(InputIterator beg, InputIterator end);
        Tries to copy elements into the queue from beg, until end is reached.
        Returns an iterator for the next element to be inserted

    size_type push_n(size_type count, const T& elem);
        Tries to insert count copies of elem into the queue. Returns the
        number of copies successfully inserted.

    template<typename... Args>
    bool emplace(Args&&... args);
        Tries to insert an object of type T constructed from args into the
        queue. Returns true if successful, false otherwise.

    template<typename... Args>
    size_type emplace_n(size_type count, Args&&... args);
        Tries to insert count objects of type T constructed from args into
        the queue. Returns the number of objects successfully inserted.

    template<typename Callback>
    bool produce(Callback&& cb);
        Tries to insert an object into the queue by calling Callback if there is
        space for an object. Returns true if there was space for an object, and
        Callback returned true. Returns false otherwise.
        Callback is an invocable with one parameter of type void*, and a return
        type of bool. Callback is expected to place a new object of type T at
        the address passed to it.

    template<typename Callback>
    size_type produce_n(size_type count, Callback&& cb);
        Tries to insert count objects into the queue by calling Callback as long
        as there is space in the queue, or until Callback returns false once.
        Returns the number of times Callback was invoked and returned true.
        Callback is an invocable with one parameter of type void*, and a return
        type of bool. Callback is expected to place a new object of type T at
        the address passed to it.

  Dequeue:
    const T* front() const;
    T* front();
        Returns a pointer to the next object in the queue, if such an object
        exists. Returns nullptr if the queue is empty.

    void discard();
        Removes the next object from the queue. This function must not be called
        if the queue is empty.

    bool pop(T& out);
        Tries to move the next object in the queue into out, if such an object
        exists. Returns true if out contains a new object. Returns false if the
        queue was empty.

    template<typename Callback>
    bool consume(Callback&& cb);
        Tries to remove an object from the queue by calling Callback and passing
        the object to it. Returns true if there was an object in the queue and
        Callback returned true. Returns false otherwise.
        Callback is an invocable with one parameter of type T*, and a return
        type of bool.

    template<typename Callback>
    size_type consume_all(Callback&& cb);
        Tries to remove all objects from the queue by calling Callback for each
        object, passing the address of each object to it, until either the queue
        is empty, or Callback returns false. Returns the number of times
        Callback was invoked and returned true.
        Callback is an invocable with one parameter of type T*, and a return
        type of bool.

******************************************************************************/
#pragma once

#include <array> // for std::array
#include <atomic> // for std::atomic<T> and std::atomic_thread_fence
#include <cstddef> // for std::byte
#include <new> // for std::launder
#include <type_traits> // for std::forward, std::is_invocable_r_v, and
                       // std::is_constructible_v
#include <functional> // for std::invoke

namespace deaod {

namespace detail {

template<typename Functor>
struct scope_guard {
    scope_guard(Functor&& f) : _f(std::forward<Functor>(f)) {}
    ~scope_guard() { std::forward<Functor>(_f)(); };

    scope_guard(const scope_guard&) = delete;
    scope_guard& operator=(const scope_guard&) = delete;

private:
    Functor&& _f;
};

} // namespace detail

template<typename T, size_t queue_size, int align_log2 = 7>
struct alignas((size_t)1 << align_log2) spsc_queue { // gcc bug 89683
    using value_type = T;
    using size_type = size_t;

    static const auto size = queue_size;
    static const auto align = size_t(1) << align_log2;

    static_assert(
        alignof(T) <= align,
        "Type T must not be more aligned than this queue"
    );

    spsc_queue() = default;

    ~spsc_queue() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        consume_all([](T*) { return true; });
    }

    spsc_queue(const spsc_queue & other) {
        auto tail = 0;

        detail::scope_guard g([&, this] {
            tail_cache = tail;
            _tail.store(tail);
        });

        auto src_tail = other._tail.load();
        auto src_head = other._head.load();

        while (src_head != src_tail) {
            new(_buffer.data() + tail * sizeof(T))
                T(*std::launder(reinterpret_cast<T*>(
                    other._buffer.data() + src_head * sizeof(T)
                )));

            tail += 1;
            src_head += 1;
            if (src_head == size) src_head = 0;
        }
    }

    spsc_queue& operator=(const spsc_queue & other) {
        if (this == &other) return *this;

        {
            auto head = _head.load();
            auto tail = _tail.load();

            detail::scope_guard g([&, this] {
                head_cache = head;
                _head.store(head);
            });

            while (head != tail) {
                auto elem = std::launder(
                    reinterpret_cast<T*>(_buffer.data() + head * sizeof(T))
                );
                elem->~T();

                head += 1;
                if (head == size) head = 0;
            }
        }

        _tail.store(0);
        head_cache = 0;
        _head.store(0);
        tail_cache = 0;

        {
            auto tail = 0;

            detail::scope_guard g([&, this] {
                tail_cache = tail;
                _tail.store(tail);
                });

            auto src_tail = other._tail.load();
            auto src_head = other._head.load();

            while (src_head != src_tail) {
                new(_buffer.data() + tail * sizeof(T))
                    T(*std::launder(reinterpret_cast<T*>(
                        other._buffer.data() + src_head * sizeof(T)
                    )));

                tail += 1;
                src_head += 1;
                if (src_head == size) src_head = 0;
            }
        }

        return *this;
    }

    bool is_empty() const {
        auto head = _head.load(std::memory_order_acquire);
        auto tail = _tail.load(std::memory_order_acquire);

        return head == tail;
    }

    bool is_full() const {
        auto head = _head.load(std::memory_order_acquire);
        auto tail = _tail.load(std::memory_order_acquire) + 1;
        if (tail == size) tail = 0;

        return head == tail;
    }

    // copies elem into queue, if theres space
    // returns true if successful, false otherwise
    bool push(const T & elem) {
        auto tail = _tail.load(std::memory_order_relaxed);
        auto next = tail + 1;
        if (next == size) next = 0;

        auto head = head_cache;
        if (next == head) {
            head = head_cache = _head.load(std::memory_order_acquire);
            if (next == head) {
                return false;
            }
        }

        new(_buffer.data() + tail * sizeof(T)) T(elem);

        _tail.store(next, std::memory_order_release);
        return true;
    }

    // tries to move elem into queue, if theres space
    // returns true if successful, false otherwise
    bool push(T && elem) {
        auto tail = _tail.load(std::memory_order_relaxed);
        auto next = tail + 1;
        if (next == size) next = 0;

        auto head = head_cache;
        if (next == head) {
            head = head_cache = _head.load(std::memory_order_acquire);
            if (next == head) {
                return false;
            }
        }

        new(_buffer.data() + tail * sizeof(T)) T(std::move(elem));

        _tail.store(next, std::memory_order_release);
        return true;
    }

    // copies elements into queue until end is reached or queue is full,
    // whichever happens first
    // returns iterator to the next element after the one that got inserted last
    template<typename InputIterator>
    InputIterator push(InputIterator beg, InputIterator end) {
        auto tail = _tail.load(std::memory_order_relaxed);

        detail::scope_guard g([&, this] {
            _tail.store(tail, std::memory_order_release);
        });

        for (; beg != end; ++beg) {
            auto next = tail + 1;
            if (next == size) next = 0;

            auto head = head_cache;
            if (next == head) {
                head = head_cache = _head.load(std::memory_order_acquire);
                if (next == head) {
                    return beg;
                }
            }

            new(_buffer.data() + tail * sizeof(T)) T(*beg);
            tail = next;
        }

        return beg;
    }

    // tries to copy count elements into the queue
    // returns the number of elements that actually got copied
    size_type push_n(size_type count, const T & elem) {
        auto tail = _tail.load(std::memory_order_relaxed);
        auto head = head_cache;
        auto free = size - (tail - head);
        if (free > size) free -= size;

        if (count >= free) {
            head = head_cache = _head.load(std::memory_order_acquire);
            free = size - (tail - head);
            if (free > size) free -= size;

            if (count >= free) {
                count = free - 1;
            }
        }

        auto next = tail + count;
        if (next >= size) next -= size;

        detail::scope_guard g([&, this] {
            _tail.store(tail, std::memory_order_release);
            });

        while (tail != next) {
            new(_buffer.data() + tail * sizeof(T)) T{ elem };

            tail += 1;
            if (tail == size) tail = 0;
        }

        return count;
    }

    // constructs an element of type T in place using Args
    // returns true if successful, false otherwise
    template<typename... Args>
    bool emplace(Args && ... args) {
        static_assert(
            std::is_constructible_v<value_type, Args...>,
            "Type T must be constructible from Args..."
        );

        auto tail = _tail.load(std::memory_order_relaxed);
        auto next = tail + 1;
        if (next == size) next = 0;

        auto head = head_cache;
        if (next == head) {
            head = head_cache = _head.load(std::memory_order_acquire);
            if (next == head) {
                return false;
            }
        }

        new(_buffer.data() + tail * sizeof(T)) T{ std::forward<Args>(args)... };

        _tail.store(next, std::memory_order_release);
        return true;
    }

    // tries to construct count elements of type T in place using Args
    // returns the number of elements that got constructed
    template<typename... Args>
    size_type emplace_n(size_type count, Args && ... args) {
        static_assert(
            std::is_constructible_v<value_type, Args...>,
            "Type T must be constructible from Args..."
        );

        auto tail = _tail.load(std::memory_order_relaxed);
        auto head = head_cache;
        auto free = size - (tail - head);
        if (free > size) free -= size;

        if (count >= free) {
            head = head_cache = _head.load(std::memory_order_acquire);
            free = size - (tail - head);
            if (free > size) free -= size;

            if (count >= free) {
                count = free - 1;
            }
        }

        auto next = tail + count;
        if (next >= size) next -= size;

        detail::scope_guard g([&, this] {
            _tail.store(tail, std::memory_order_release);
        });

        while (tail != next) {
            new(_buffer.data() + tail * sizeof(T)) T{ args... };

            tail += 1;
            if (tail == size) tail = 0;
        }

        return count;
    }

    // Callback is an invocable that takes void* and returns bool
    // Callback must use placement new to construct an object of type T at the
    // pointer passed to it. If it cannot do so, it must return false. If it
    // returns false, an object of type T must not have been constructed.
    // 
    // This function returns true if there was space for at least one element,
    // and Callback returned true. Otherwise, false will be returned.
    template<typename Callback>
    bool produce(Callback && cb) {
        static_assert(
            std::is_invocable_r_v<bool, Callback&&, void*>,
            "Callback must return bool, and take void*"
        );

        auto tail = _tail.load(std::memory_order_relaxed);
        auto next = tail + 1;
        if (next == size) next = 0;

        auto head = head_cache;
        if (next == head) {
            head = head_cache = _head.load(std::memory_order_acquire);
            if (next == head) {
                return false;
            }
        }

        void* storage = _buffer.data() + tail * sizeof(T);
        if (std::invoke(std::forward<Callback>(cb), storage)) {
            _tail.store(next, std::memory_order_release);
            return true;
        }

        return false;
    }

    // Callback is an invocable that takes void* and returns bool
    // Callback must use placement new to construct an object of type T at the
    // pointer passed to it. If it cannot do so, it must return false. If it
    // returns false, an object of type T must not have been constructed.
    // 
    // This function tries to construct count elements by calling Callback for
    // each address where an object can be constructed. This function returns
    // the number of elements that were successfully constructed, that is the
    // number of times Callback returned true.
    template<typename Callback>
    size_type produce_n(size_type count, Callback && cb) {
        static_assert(
            std::is_invocable_r_v<bool, Callback&&, void*>,
            "Callback must return bool, and take void*"
        );

        auto tail = _tail.load(std::memory_order_relaxed);
        auto head = head_cache;
        auto free = size - (tail - head);
        if (free > size) free -= size;

        if (count >= free) {
            head = head_cache = _head.load(std::memory_order_acquire);
            free = size - (tail - head);
            if (free > size) free -= size;

            if (count >= free) {
                count = free - 1;
            }
        }

        auto next = tail + count;
        if (next >= size) next -= size;

        detail::scope_guard g([&, this] {
            _tail.store(tail, std::memory_order_release);
        });

        while (tail != next) {
            void* storage = _buffer.data() + tail * sizeof(T);
            if (!std::invoke(cb, storage)) {
                auto ret = next - tail;
                if (ret < 0) ret += size;
                return ret;
            }

            tail += 1;
            if (tail == size) tail = 0;
        }

        return count;
    }

    // Returns a pointer to the next element that can be dequeued, or nullptr
    // if the queue is empty.
    const T* front() const {
        auto head = _head.load(std::memory_order_relaxed);
        auto tail = tail_cache;

        if (head == tail) {
            tail = tail_cache = _tail.load(std::memory_order_acquire);
            if (head == tail) {
                return nullptr;
            }
        }

        return std::launder(
            reinterpret_cast<const T*>(_buffer.data() + head * sizeof(T))
        );
    }

    // Returns a pointer to the next element that can be dequeued, or nullptr
    // if the queue is empty.
    T* front() {
        auto head = _head.load(std::memory_order_relaxed);
        auto tail = tail_cache;

        if (head == tail) {
            tail = tail_cache = _tail.load(std::memory_order_acquire);
            if (head == tail) {
                return nullptr;
            }
        }

        return std::launder(
            reinterpret_cast<T*>(_buffer.data() + head * sizeof(T))
        );
    }

    // Discards the next element to be dequeued. The queue must contain at
    // least one element before calling this function.
    void discard() {
        auto head = _head.load(std::memory_order_relaxed);

        auto elem = std::launder(
            reinterpret_cast<T*>(_buffer.data() + head * sizeof(T))
        );
        elem->~T();

        auto next = head + 1;
        if (next == size) next = 0;
        _head.store(next, std::memory_order_release);
    }

    // tries to move the next element to be dequeued into out.
    // Returns true if out was assigned to, false otherwise.
    bool pop(T & out) {
        auto head = _head.load(std::memory_order_relaxed);
        auto tail = tail_cache;

        if (head == tail) {
            tail = tail_cache = _tail.load(std::memory_order_acquire);
            if (head == tail) {
                return false;
            }
        }

        auto elem = std::launder(
            reinterpret_cast<T*>(_buffer.data() + head * sizeof(T))
        );

        out = std::move(*elem);
        elem->~T();

        auto next = head + 1;
        if (next == size) next = 0;
        _head.store(next, std::memory_order_release);
        return true;
    }

    // Callback is an invocable that takes T* and returns bool
    //
    // This function calls Callback with the address of the next element to be
    // dequeued, if the queue is not empty. If Callback returns true, the
    // element is removed from the queue and this function returns true.
    // Otherwise this function returns false.
    template<typename Callback>
    bool consume(Callback && cb) {
        static_assert(
            std::is_invocable_r_v<bool, Callback&&, T*>,
            "Callback must return bool, and take T*"
        );

        auto head = _head.load(std::memory_order_relaxed);
        auto tail = tail_cache;

        if (head == tail) {
            tail = tail_cache = _tail.load(std::memory_order_acquire);
            if (head == tail) {
                return false;
            }
        }

        auto elem = std::launder(
            reinterpret_cast<T*>(_buffer.data() + head * sizeof(T))
        );

        if (std::invoke(std::forward<Callback>(cb), elem)) {
            elem->~T();
            auto next = head + 1;
            if (next == size) next = 0;
            _head.store(next, std::memory_order_release);
            return true;
        }

        return false;
    }

    // Callback is an invocable that takes T* and returns bool
    //
    // This function calls Callback for each element currently in the queue,
    // with the address of that element. If Callback returns true, the element
    // is removed from the queue. If Callback returns false, the element is not
    // removed, and this function returns. This function always returns the
    // number of times Callback returned true.
    template<typename Callback>
    size_type consume_all(Callback && cb) {
        static_assert(
            std::is_invocable_r_v<bool, Callback&&, T*>,
            "Callback must return bool, and take T*"
        );

        auto head = _head.load(std::memory_order_relaxed);
        auto tail = tail_cache = _tail.load(std::memory_order_acquire);
        auto old_head = head;

        detail::scope_guard g([&, this] {
            _head.store(head, std::memory_order_release);
        });

        while (head != tail) {
            auto elem = std::launder(
                reinterpret_cast<T*>(_buffer.data() + head * sizeof(T))
            );

            if (!std::invoke(cb, elem)) {
                break;
            }

            elem->~T();
            head += 1;
            if (head == size) head = 0;
        }

        ptrdiff_t ret = head - old_head;
        if (ret < 0) ret += size;
        return ret;
    }

private:
    alignas(align) std::array<std::byte, size * sizeof(T)> _buffer;

    alignas(align) std::atomic<size_t> _tail{ 0 };
    mutable size_t head_cache{ 0 };

    alignas(align) std::atomic<size_t> _head{ 0 };
    mutable size_t tail_cache{ 0 };
};

} // namespace deaod
